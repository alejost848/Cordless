#include "voice/voice_audio.h"
#include "voice/voice_crypto.h"
#if VOICE_DAVE_ENABLE
#include "voice/dave_frame.h"
#endif
#include "console.h"
#include "net_common.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <opus/opus.h>
#include <speex/speex_preprocess.h>
#include <psp2/audioout.h>
#include <psp2/audioin.h>
#include <psp2/kernel/threadmgr.h>

// Voice audio pipeline (Vita-only: Opus + sceAudio + threads). Transport crypto
// and RTP framing live in voice_crypto.c.

int voice_audio_selftest(void) { return voice_crypto_selftest(); }

// RX thread: drains the non-blocking UDP socket (net_lock held only per recv),
// decrypts + Opus-decodes per SSRC, mixes into 20ms stereo frames for sceAudioOut.

#define VA_FRAME_SAMPLES 960  // 20ms @ 48kHz, per channel
#ifdef CORDLESS_PLUGIN
#define VA_MAX_SPEAKERS  4
#define VA_JITTER_FRAMES 3
#else
#define VA_MAX_SPEAKERS  8
#define VA_JITTER_FRAMES 8
#endif
#define VA_JITTER_PREBUF 2   // 40ms pre-buffer on utterance start
#define VA_PLC_MAX_FRAMES 5  // up to 100ms of Opus PLC on packet loss

static volatile int      s_run;
static volatile int      s_muted;
static volatile int      s_deafened;
static volatile int      s_tx_speaking;
static volatile unsigned s_rx_tick;
static int           s_udp = -1;  // borrowed from voice.c
static int           s_aport = -1;  // sceAudioOut port
static int           s_mode_id;
static unsigned char s_key[32];
static unsigned      s_ssrc;  // our own ssrc (ignore our loopback)
static void        (*s_on_speaking)(int);
static SceUID        s_rx_thread = -1;

typedef struct {
    unsigned     ssrc;
    OpusDecoder *dec;
    unsigned     last_tick;
    unsigned     speaking_until;
    uint16_t     last_seq;
    uint8_t      has_seq;
    uint8_t      r_idx;
    uint8_t      w_idx;
    uint8_t      count;
    uint8_t      priming;
    uint8_t      plc_streak;
    int16_t      pcm[VA_JITTER_FRAMES][VA_FRAME_SAMPLES * 2];
} spk_slot;
static spk_slot *s_spk = NULL;

void voice_audio_set_muted(int muted)       { s_muted = muted ? 1 : 0; }
int  voice_audio_is_muted(void)             { return s_muted; }
void voice_audio_set_deafened(int deafened) { s_deafened = deafened ? 1 : 0; }
int  voice_audio_is_deafened(void)          { return s_deafened; }
int  voice_audio_is_tx_speaking(void)       { return (!s_muted && !s_deafened && s_tx_speaking) ? 1 : 0; }

int voice_audio_ssrc_speaking(unsigned ssrc)
{
    if (!ssrc || s_deafened || !s_spk) return 0;
    unsigned tick = s_rx_tick;
    for (int i = 0; i < VA_MAX_SPEAKERS; i++) {
        if (s_spk[i].dec && s_spk[i].ssrc == ssrc &&
            s_spk[i].speaking_until != 0 && (int)(s_spk[i].speaking_until - tick) >= 0)
            return 1;
    }
    return 0;
}

unsigned voice_audio_speaking_mask(void)
{
    unsigned mask = 0;
    unsigned tick = s_rx_tick;
    if (!s_deafened && s_spk) {
        for (int i = 0; i < VA_MAX_SPEAKERS; i++) {
            if (s_spk[i].dec && s_spk[i].speaking_until != 0 &&
                (int)(s_spk[i].speaking_until - tick) >= 0)
                mask |= (1u << i);
        }
    }
    if (voice_audio_is_tx_speaking())
        mask |= (1u << 8);
    return mask;
}

#if VOICE_DAVE_ENABLE
// DAVE inner-layer key resolvers (from voice.c) + derived-key caches, so the hooks
// are called only on a generation or epoch change.
static const voice_dave_hooks *s_dave_hooks;
static volatile unsigned s_rx_epoch_gen;
static volatile unsigned s_tx_epoch_gen;

void voice_audio_set_dave(const voice_dave_hooks *h) { s_dave_hooks = h; }
void voice_audio_invalidate_dave_keys(int tx_too)
{
    s_rx_epoch_gen++;
    if (tx_too) s_tx_epoch_gen++;
}

// Our TX media key at the current generation.
static uint32_t s_tx_nonce;  // monotonic 32-bit truncated sync nonce
static int      s_tx_key_gen = -1;
static int      s_tx_key_ok;
static unsigned s_tx_epoch_seen;
static uint8_t  s_tx_key[16];

// Per-remote-SSRC RX media-key cache (ssrc==0 => unused slot).
// Holds one prior epoch's key per SSRC so in-flight transition frames still decrypt.
static struct {
    unsigned ssrc;
    int      gen;
    uint8_t  key[16];
    int      ok;
    uint8_t  prev_key[16];
    int      prev_ok;
    unsigned last_tick;
} s_rxkey[VA_MAX_SPEAKERS];
static unsigned s_rx_epoch_seen;

static void rx_sync_epoch(void)
{
    unsigned eg = s_rx_epoch_gen;
    if (eg == s_rx_epoch_seen) return;
    for (int i = 0; i < VA_MAX_SPEAKERS; i++) {
        if (s_rxkey[i].ssrc && s_rxkey[i].ok) {
            memcpy(s_rxkey[i].prev_key, s_rxkey[i].key, 16);
            s_rxkey[i].prev_ok = 1;
        }
        s_rxkey[i].ok = 0;
        s_rxkey[i].gen = -1;
    }
    s_rx_epoch_seen = eg;
}

static int rxkey_get(unsigned ssrc, uint8_t gen, uint8_t out[16],
                     uint8_t prev_out[16], int *has_prev)
{
    rx_sync_epoch();
    if (has_prev) *has_prev = 0;
    if (!s_dave_hooks || !s_dave_hooks->sender_key) return 0;
    unsigned tick = s_rx_tick;
    int slot = -1;
    for (int i = 0; i < VA_MAX_SPEAKERS; i++)
        if (s_rxkey[i].ssrc == ssrc) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < VA_MAX_SPEAKERS; i++)
            if (s_rxkey[i].ssrc == 0) {
                slot = i;
                memset(&s_rxkey[i], 0, sizeof(s_rxkey[i]));
                s_rxkey[i].ssrc = ssrc;
                s_rxkey[i].gen = -1;
                break;
            }
    if (slot < 0) {
        int lru = 0;
        unsigned oldest = (unsigned)(tick - s_rxkey[0].last_tick);
        for (int i = 1; i < VA_MAX_SPEAKERS; i++) {
            unsigned age = (unsigned)(tick - s_rxkey[i].last_tick);
            if (age > oldest) { oldest = age; lru = i; }
        }
        slot = lru;
        memset(&s_rxkey[slot], 0, sizeof(s_rxkey[slot]));
        s_rxkey[slot].ssrc = ssrc;
        s_rxkey[slot].gen = -1;
    }
    s_rxkey[slot].last_tick = tick;
    if (!s_rxkey[slot].ok || s_rxkey[slot].gen != (int)gen) {  // (re)derive only on change
        s_rxkey[slot].ok  = s_dave_hooks->sender_key(ssrc, gen, s_rxkey[slot].key);
        s_rxkey[slot].gen = (int)gen;
    }
    if (has_prev && prev_out && s_rxkey[slot].prev_ok) {
        memcpy(prev_out, s_rxkey[slot].prev_key, 16);
        *has_prev = 1;
    }
    if (!s_rxkey[slot].ok) return 0;
    memcpy(out, s_rxkey[slot].key, 16);
    return 1;
}
#endif  // VOICE_DAVE_ENABLE

static spk_slot *speaker_for(unsigned ssrc, unsigned tick)
{
    for (int i = 0; i < VA_MAX_SPEAKERS; i++) {
        if (s_spk[i].dec && s_spk[i].ssrc == ssrc) {
            s_spk[i].last_tick = tick;
            return &s_spk[i];
        }
    }
    for (int i = 0; i < VA_MAX_SPEAKERS; i++) {
        if (!s_spk[i].dec) {
            int err = 0;
            OpusDecoder *d = opus_decoder_create(48000, 2, &err);
            if (!d) {
                console_logf("[voice] opus decoder create failed %d", err);
                return NULL;
            }
            memset(&s_spk[i], 0, sizeof(s_spk[i]));
            s_spk[i].dec = d;
            s_spk[i].ssrc = ssrc;
            s_spk[i].last_tick = tick;
            s_spk[i].priming = 1;
            return &s_spk[i];
        }
    }
    int lru = 0;
    unsigned oldest = (unsigned)(tick - s_spk[0].last_tick);
    for (int i = 1; i < VA_MAX_SPEAKERS; i++) {
        unsigned age = (unsigned)(tick - s_spk[i].last_tick);
        if (age > oldest) { oldest = age; lru = i; }
    }
    OpusDecoder *d = s_spk[lru].dec;
    opus_decoder_ctl(d, OPUS_RESET_STATE);
    s_spk[lru].ssrc = ssrc;
    s_spk[lru].last_tick = tick;
    s_spk[lru].speaking_until = 0;
    s_spk[lru].r_idx = 0;
    s_spk[lru].w_idx = 0;
    s_spk[lru].count = 0;
    s_spk[lru].has_seq = 0;
    s_spk[lru].last_seq = 0;
    s_spk[lru].priming = 1;
    s_spk[lru].plc_streak = 0;
    return &s_spk[lru];
}

static volatile int      s_cue_kind = -1;
static volatile unsigned s_cue_seq = 0;

void voice_audio_play_cue(int kind)
{
    s_cue_kind = kind;
    s_cue_seq++;
}

static void mix_cue_into(int16_t *mix)
{
    static unsigned seen_seq = 0;
    static int kind = -1;
    static int delay_left = 0;
    static int frames_left = 0;
    static int prev_note = -1;
    static float phase = 0.0f;

    unsigned seq = s_cue_seq;
    if (seq != seen_seq) {
        seen_seq = seq;
        kind = s_cue_kind;
        delay_left = (kind == 0) ? 2 : 0;   // let sceAudioOut prime before connect chime
        frames_left = (kind == 0) ? 9 : 7;  // 180ms connect chime, 140ms mute/unmute cue
        prev_note = -1;
        phase = 0.0f;
    }
    if (delay_left > 0) {
        delay_left--;
        return;
    }
    if (frames_left <= 0) return;

    int total = (kind == 0) ? 9 : 7;
    int split = (kind == 0) ? 4 : 3;
    int frame_idx = total - frames_left;
    int note = (frame_idx < split) ? 0 : 1;
    if (note != prev_note) {
        prev_note = note;
        phase = 0.0f;
    }

    float freq;
    if (kind == 0)      freq = (note == 0) ? 523.25f : 783.99f;  // C5 -> G5 (connect)
    else if (kind == 1) freq = (note == 0) ? 783.99f : 587.33f;  // G5 -> D5 (mute)
    else                freq = (note == 0) ? 587.33f : 783.99f;  // D5 -> G5 (unmute)

    float amp = (kind == 0) ? 4000.0f : 4500.0f;
    float phase_step = 2.0f * 3.14159265f * freq / 48000.0f;

    // Sample position within the current note for smooth attack/release ramps.
    int note_frames = (note == 0) ? split : (total - split);
    int note_frame_idx = (note == 1) ? (frame_idx - split) : frame_idx;
    int note_len = note_frames * VA_FRAME_SAMPLES;
    int att_samples = 384;                                 // 8ms fade-in
    int rel_samples = (note == 1) ? 1440 : 600;            // 30ms / 12.5ms fade-out

    for (int i = 0; i < VA_FRAME_SAMPLES; i++) {
        int npos = note_frame_idx * VA_FRAME_SAMPLES + i;
        float env = 1.0f;
        if (npos < att_samples)
            env = (float)npos / (float)att_samples;
        else if (npos > note_len - rel_samples)
            env = (float)(note_len - npos) / (float)rel_samples;
        if (env < 0.0f) env = 0.0f;

        int16_t s = (int16_t)(sinf(phase) * amp * env);
        phase += phase_step;
        if (phase > 6.2831853f) phase -= 6.2831853f;
        int l = (int)mix[i * 2] + s;
        int r = (int)mix[i * 2 + 1] + s;
        mix[i * 2]     = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
        mix[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }
    frames_left--;
}

static void spk_push_pcm(spk_slot *sp, int got)
{
    if (got <= 0) return;
    int16_t *dst = sp->pcm[sp->w_idx];
    if (got < VA_FRAME_SAMPLES)
        memset(dst + got * 2, 0, (size_t)(VA_FRAME_SAMPLES - got) * 2 * sizeof(int16_t));
    sp->w_idx = (uint8_t)((sp->w_idx + 1) % VA_JITTER_FRAMES);
    if (sp->count < VA_JITTER_FRAMES)
        sp->count++;
    else
        sp->r_idx = (uint8_t)((sp->r_idx + 1) % VA_JITTER_FRAMES);
}

static int rx_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
#ifdef CORDLESS_PLUGIN
    int rx_logged = 0;
#endif
    static uint8_t pkt[1600], plain[1600];
    // Ping-pong output buffers: sceAudioOutOutput may DMA from the submitted
    // buffer after returning until the next sceAudioOutOutput completes.
    static int16_t mix_buf[2][VA_FRAME_SAMPLES * 2];
    int mix_idx = 0;
    unsigned dbg_pkts = 0, dbg_open_ok = 0, dbg_dave_nokey = 0, dbg_dave_decfail = 0, dbg_opus_ok = 0;

    while (s_run) {
        unsigned tick = ++s_rx_tick;
        for (int guard = 0; guard < 64 && s_run; guard++) {  // drain this tick's packets
            int n = (int)recv(s_udp, pkt, sizeof pkt, 0);  // O_NONBLOCK: returns fast
            if (n <= 0) break;
            dbg_pkts++;
            if (n < 12) continue;
            int ptype = pkt[1] & 0x7F;
            if (ptype >= 72 && ptype <= 76) continue;  // RTCP, not Opus
            uint16_t seq = (uint16_t)(((unsigned)pkt[2] << 8) | pkt[3]);
            unsigned ssrc = ((unsigned)pkt[8] << 24) | ((unsigned)pkt[9] << 16) |
                            ((unsigned)pkt[10] << 8) | pkt[11];
            if (ssrc == s_ssrc) continue;  // our own loopback
            int plen = voice_packet_open(s_mode_id, s_key, pkt, (size_t)n, plain, sizeof plain);
            if (plen < 0) continue;  // auth fail / malformed
            dbg_open_ok++;
            const uint8_t *opus = plain; int olen = plen;
            if (pkt[0] & 0x10) {  // extension body is encrypted
                size_t hl = voice_rtp_header_len(pkt, (size_t)n);
                if (hl >= 4) {
                    int words = ((int)pkt[hl - 2] << 8) | pkt[hl - 1];
                    int skip = words * 4;
                    if (skip >= 0 && skip <= olen) { opus += skip; olen -= skip; }
                }
            }
            if (olen <= 0) continue;
#if VOICE_DAVE_ENABLE
            // Inner E2EE: if this is a DAVE frame (0xFAFA trailer) with keys,
            // unwrap to plaintext Opus; a non-DAVE payload passes through.
            static uint8_t dave_plain[1600];
            if (s_dave_hooks && s_dave_hooks->sender_key) {
                uint32_t fnonce;
                if (dave_frame_peek_nonce(opus, (size_t)olen, &fnonce) == 0) {
                    uint8_t key[16], prev_key[16];
                    int has_prev = 0;
                    int has_cur = rxkey_get(ssrc, (uint8_t)(fnonce >> 24),
                                            key, prev_key, &has_prev);
                    int dl = -1;
                    if (has_cur)
                        dl = dave_frame_decrypt(key, opus, (size_t)olen,
                                                dave_plain, sizeof dave_plain, NULL);
                    if (dl <= 0 && has_prev)
                        dl = dave_frame_decrypt(prev_key, opus, (size_t)olen,
                                                dave_plain, sizeof dave_plain, NULL);
                    if (!has_cur && !has_prev) { dbg_dave_nokey++; continue; }
                    if (dl <= 0) {
                        static int s_logged_df = 0;
                        if (!s_logged_df) {
                            s_logged_df = 1;
                            int supp = (olen >= 3) ? opus[olen - 3] : -1;
                            console_logf("[dave-dbg] ssrc=%u olen=%d supp=%d nonce=0x%08x gen=%u has_cur=%d has_prev=%d k0=%02x%02x%02x%02x",
                                         ssrc, olen, supp, fnonce, (unsigned)(fnonce >> 24),
                                         has_cur, has_prev, key[0], key[1], key[2], key[3]);
                        }
                        dbg_dave_decfail++;
                        continue;
                    }  // E2EE auth fail: drop
                    opus = dave_plain; olen = dl;
                }
            }
#endif
            if (s_deafened) continue;
            spk_slot *sp = speaker_for(ssrc, tick);
            if (!sp) continue;
            int is_sil = (olen == 3 && opus[0] == 0xF8 && opus[1] == 0xFF && opus[2] == 0xFE);
            if (sp->has_seq) {
                int16_t sdiff = (int16_t)(seq - sp->last_seq);
                if (sdiff <= 0 && sdiff > -1000) {
                    continue;  // stale or duplicate out-of-order packet
                }
                if (!is_sil && sdiff == 2) {
                    // Single lost packet: recover using Opus in-band FEC from current packet.
                    int fgot = opus_decode(sp->dec, opus, olen, sp->pcm[sp->w_idx], VA_FRAME_SAMPLES, 1);
                    if (fgot > 0) spk_push_pcm(sp, fgot);
                } else if (!is_sil && sdiff == 3) {
                    // Two lost packets: 1 PLC frame + 1 in-band FEC frame.
                    int pgot = opus_decode(sp->dec, NULL, 0, sp->pcm[sp->w_idx], VA_FRAME_SAMPLES, 0);
                    if (pgot > 0) spk_push_pcm(sp, pgot);
                    int fgot = opus_decode(sp->dec, opus, olen, sp->pcm[sp->w_idx], VA_FRAME_SAMPLES, 1);
                    if (fgot > 0) spk_push_pcm(sp, fgot);
                }
            }
            sp->last_seq = seq;
            sp->has_seq = 1;
            if (!is_sil) sp->speaking_until = tick + 12;
            else sp->speaking_until = 0;
            int got = opus_decode(sp->dec, opus, olen, sp->pcm[sp->w_idx], VA_FRAME_SAMPLES, 0);
            if (got <= 0) continue;
            dbg_opus_ok++;
            spk_push_pcm(sp, got);
        }
        int16_t *mix = mix_buf[mix_idx];
        mix_idx ^= 1;
        memset(mix, 0, sizeof mix_buf[0]);
        if (s_deafened) {
            for (int i = 0; i < VA_MAX_SPEAKERS; i++) {
                s_spk[i].r_idx = s_spk[i].w_idx = s_spk[i].count = 0;
                s_spk[i].speaking_until = 0;
                s_spk[i].priming = 1;
                s_spk[i].plc_streak = 0;
            }
        } else {
            static int16_t plc_buf[VA_FRAME_SAMPLES * 2];
            for (int s = 0; s < VA_MAX_SPEAKERS; s++) {
                spk_slot *sp = &s_spk[s];
                if (!sp->dec) continue;
                const int16_t *src = NULL;
                if (sp->count > 0) {
                    if (sp->priming && sp->count < VA_JITTER_PREBUF &&
                        (int)(sp->last_tick + 2 - tick) >= 0) {
                        continue;  // wait 1-2 ticks to build 40ms jitter cushion
                    }
                    sp->priming = 0;
                    sp->plc_streak = 0;
                    src = sp->pcm[sp->r_idx];
                    sp->r_idx = (uint8_t)((sp->r_idx + 1) % VA_JITTER_FRAMES);
                    sp->count--;
                } else if (!sp->priming && sp->speaking_until != 0 &&
                           (int)(sp->speaking_until - tick) >= 0 &&
                           sp->plc_streak < VA_PLC_MAX_FRAMES) {
                    int got = opus_decode(sp->dec, NULL, 0, plc_buf, VA_FRAME_SAMPLES, 0);
                    sp->plc_streak++;
                    if (got > 0) {
                        if (got < VA_FRAME_SAMPLES)
                            memset(plc_buf + got * 2, 0,
                                   (size_t)(VA_FRAME_SAMPLES - got) * 2 * sizeof(int16_t));
                        src = plc_buf;
                    }
                } else if (sp->speaking_until == 0 || (int)(sp->speaking_until - tick) < 0) {
                    sp->priming = 1;
                    sp->plc_streak = 0;
                }
                if (!src) continue;
                for (int i = 0; i < VA_FRAME_SAMPLES * 2; i++) {
                    int v = (int)mix[i] + (int)src[i];
                    mix[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
                }
            }
        }
        mix_cue_into(mix);
        if (s_aport >= 0) sceAudioOutOutput(s_aport, mix);  // blocks ~20ms -> paces us
        else sceKernelDelayThread(20 * 1000);
#ifdef CORDLESS_PLUGIN
        if (!rx_logged) { rx_logged = 1; console_logf("[voice] rx loop ready (aport=%d)", s_aport); }
#endif
        if ((tick % 250) == 0) {
            console_logf("[voice-rx] pkts=%u open=%u nokey=%u decfail=%u opus=%u",
                         dbg_pkts, dbg_open_ok, dbg_dave_nokey, dbg_dave_decfail, dbg_opus_ok);
            dbg_pkts = dbg_open_ok = dbg_dave_nokey = dbg_dave_decfail = dbg_opus_ok = 0;
        }
    }
#ifdef CORDLESS_PLUGIN
    extern int vitasdk_delete_thread_reent(SceUID thid);
    vitasdk_delete_thread_reent(0);
#endif
    return 0;
}

// TX: Vita mic (16kHz mono) upsampled to 48kHz, framed to 20ms, energy-VAD gated,
// Opus-encoded -> sealed -> UDP. Five 0xF8FFFE silence frames end each utterance.

#define VA_MIC_GRAIN        256     // sceAudioIn VOICE grain @ 16kHz (16ms)
#define VA_TX_FRAME_SAMPLES 320     // 20ms @ 16kHz native Opus wideband
#define VA_VAD_MS2          80000   // mean-square energy gate (attack)
#define VA_VAD_HOLD_MS2     30000   // lower hold gate while speaking so soft syllables don't clip
#define VA_VAD_HANG         25      // quiet frames before we stop (~500ms)

static SceUID               s_tx_thread = -1;
static int                  s_mic = -1;
static OpusEncoder         *s_enc;
static SpeexPreprocessState *s_denoise;

// Stub for preprocess.c when echo cancellation (mdf.c) is not linked.
void speex_echo_get_residual(void *st, float *Yout, int len)
{
    (void)st; (void)Yout; (void)len;
}

static void tx_send(const uint8_t *payload, int len, uint16_t *seq, uint32_t *ts, uint32_t *ctr)
{
    uint8_t hdr[12];
    static uint8_t pkt[1400];
#if VOICE_DAVE_ENABLE
    // Inner E2EE: wrap the Opus payload with the DAVE frame codec at our per-
    // generation key before transport seal; passthrough until a key is established.
    static uint8_t wrapped[1400];
    if (s_dave_hooks && s_dave_hooks->my_key) {
        unsigned eg = s_tx_epoch_gen;
        if (eg != s_tx_epoch_seen) {
            s_tx_epoch_seen = eg;
            s_tx_key_ok = 0;
            s_tx_key_gen = -1;
            s_tx_nonce = 0;
        }
        uint8_t gen = (uint8_t)(s_tx_nonce >> 24);
        if (!s_tx_key_ok || s_tx_key_gen != (int)gen) {
            s_tx_key_ok  = s_dave_hooks->my_key(gen, s_tx_key);
            s_tx_key_gen = (int)gen;
        }
        if (s_tx_key_ok) {
            int wl = dave_frame_encrypt(s_tx_key, s_tx_nonce, payload, (size_t)len,
                                        wrapped, sizeof wrapped);
            if (wl > 0) { payload = wrapped; len = wl; s_tx_nonce++; }
        }
    }
#endif
    voice_rtp_build(hdr, *seq, *ts, s_ssrc);
    int pn = voice_packet_seal(s_mode_id, s_key, hdr, 12, *ctr, payload, (size_t)len, pkt, sizeof pkt);
    if (pn > 0) send(s_udp, pkt, (size_t)pn, 0);
    *seq = (uint16_t)(*seq + 1); *ts += VA_FRAME_SAMPLES; (*ctr)++;  // ctr (nonce) never reused
}

static void tx_silence(uint16_t *seq, uint32_t *ts, uint32_t *ctr)
{
    static const uint8_t sil[3] = { 0xF8, 0xFF, 0xFE };
    for (int i = 0; i < 5; i++) tx_send(sil, 3, seq, ts, ctr);
}

static int tx_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
#ifdef CORDLESS_PLUGIN
    int tx_logged = 0;
#endif
    static int16_t in16[VA_MIC_GRAIN];
    static int16_t acc[2048]; int accn = 0;
    static int16_t pre_roll[VA_TX_FRAME_SAMPLES]; int has_preroll = 0;
    static uint8_t opus[1024];
    uint16_t seq = 0; uint32_t ts = 0, ctr = 1;
    int speaking = 0, quiet = 0;
    float hp_x1 = 0.0f, hp_y1 = 0.0f;
    unsigned tx_frames = 0, tx_active = 0, tx_sent = 0;
    long long tx_max_e = 0;

    while (s_run) {
        if (s_mic < 0 || !s_enc) { sceKernelDelayThread(20 * 1000); continue; }
        if (sceAudioInInput(s_mic, in16) < 0) { sceKernelDelayThread(10 * 1000); continue; }
        // 1-pole DC/rumble high-pass at ~80 Hz (16kHz native) + 2x mic preamp.
        for (int i = 0; i < VA_MIC_GRAIN && accn < (int)(sizeof(acc) / sizeof(acc[0])); i++) {
            float x = (float)in16[i];
            float y = 0.97f * (hp_y1 + x - hp_x1);
            hp_x1 = x;
            hp_y1 = y;
            int cur = (int)(y * 2.0f);
            if (cur > 32767) cur = 32767;
            else if (cur < -32768) cur = -32768;
            acc[accn++] = (int16_t)cur;
        }
        while (accn >= VA_TX_FRAME_SAMPLES && s_run) {
            if (s_denoise) speex_preprocess_run(s_denoise, acc);
            long long e = 0;
            for (int i = 0; i < VA_TX_FRAME_SAMPLES; i++) {
                int v = (acc[i] * 135) / 100; // +2.6 dB makeup with soft ceiling
                if (v > 24000) v = 24000 + (v - 24000) / 3;
                else if (v < -24000) v = -24000 + (v + 24000) / 3;
                if (v > 32000) v = 32000;
                else if (v < -32000) v = -32000;
                acc[i] = (int16_t)v;
                e += (long long)v * v;
            }
            long long ms2 = e / VA_TX_FRAME_SAMPLES;
            if (ms2 > tx_max_e) tx_max_e = ms2;
            tx_frames++;
            long long thresh = speaking ? VA_VAD_HOLD_MS2 : VA_VAD_MS2;
            int active = (s_muted || s_deafened) ? 0 : (ms2 > thresh);
            if (active) tx_active++;
            if (active || (speaking && quiet < VA_VAD_HANG)) {
                if (!speaking) {
                    speaking = 1; s_tx_speaking = 1; quiet = 0;
                    if (s_on_speaking) s_on_speaking(1);
                    if (has_preroll) {
                        int pn = opus_encode(s_enc, pre_roll, VA_TX_FRAME_SAMPLES, opus, sizeof opus);
                        if (pn > 0) { tx_send(opus, pn, &seq, &ts, &ctr); tx_sent++; }
                        has_preroll = 0;
                    }
                }
                quiet = active ? 0 : quiet + 1;
                int n = opus_encode(s_enc, acc, VA_TX_FRAME_SAMPLES, opus, sizeof opus);
#ifdef CORDLESS_PLUGIN
                if (!tx_logged && n > 0) {
                    tx_logged = 1;
                    console_logf("[voice] tx first opus ok (n=%d)", n);
                }
#endif
                if (n > 0) { tx_send(opus, n, &seq, &ts, &ctr); tx_sent++; }
            } else if (speaking) {  // trailing quiet -> stop
                tx_silence(&seq, &ts, &ctr);
                speaking = 0; s_tx_speaking = 0; quiet = 0;
                if (s_on_speaking) s_on_speaking(0);
                memcpy(pre_roll, acc, sizeof(pre_roll));
                has_preroll = 1;
            } else {
                memcpy(pre_roll, acc, sizeof(pre_roll));
                has_preroll = 1;
            }
            if ((tx_frames % 250) == 0) {
                console_logf("[voice-tx] frames=%u active=%u sent=%u dave_tx_ok=%d max_ms2=%lld",
                             tx_frames, tx_active, tx_sent, s_tx_key_ok, tx_max_e);
                tx_active = tx_sent = 0; tx_max_e = 0;
            }
            memmove(acc, acc + VA_TX_FRAME_SAMPLES, (accn - VA_TX_FRAME_SAMPLES) * sizeof(acc[0]));
            accn -= VA_TX_FRAME_SAMPLES;
        }
    }
    if (speaking) { tx_silence(&seq, &ts, &ctr); if (s_on_speaking) s_on_speaking(0); }
    s_tx_speaking = 0;
#ifdef CORDLESS_PLUGIN
    extern int vitasdk_delete_thread_reent(SceUID thid);
    vitasdk_delete_thread_reent(0);
#endif
    return 0;
}

int voice_audio_start(int udp_fd, unsigned ssrc, const unsigned char secret_key[32],
                      const char *mode, void (*on_speaking)(int on))
{
    if (udp_fd < 0) return -1;
    s_udp = udp_fd;
    s_ssrc = ssrc;
    memcpy(s_key, secret_key, 32);
    s_mode_id = voice_crypto_mode_id(mode);
    s_on_speaking = on_speaking;
    s_tx_speaking = 0;
    s_rx_tick = 0;
    if (!s_spk) s_spk = (spk_slot *)calloc(VA_MAX_SPEAKERS, sizeof(spk_slot));
    if (!s_spk) return -1;
    memset(s_spk, 0, VA_MAX_SPEAKERS * sizeof(spk_slot));
#if VOICE_DAVE_ENABLE
    memset(s_rxkey, 0, sizeof s_rxkey);
    s_rx_epoch_seen = s_rx_epoch_gen;
    s_tx_epoch_seen = s_tx_epoch_gen;
    s_tx_nonce = 0; s_tx_key_gen = -1; s_tx_key_ok = 0;
#endif

    // Non-blocking so RX recv never holds net_lock across a wait.
    int fl = fcntl(s_udp, F_GETFL, 0);
    if (fl >= 0) fcntl(s_udp, F_SETFL, fl | O_NONBLOCK);

    s_aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VA_FRAME_SAMPLES,
                                  48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (s_aport < 0) console_logf("[voice] audio out open failed 0x%08x", (unsigned)s_aport);

    s_run = 1;
#ifdef CORDLESS_PLUGIN
    const SceSize rx_stack = 0x10000;
    const int rx_prio = 0x60;
    const int rx_aff = 0;
#else
    const SceSize rx_stack = 0x10000;
    const int rx_prio = 0x40;
    const int rx_aff = SCE_KERNEL_CPU_MASK_USER_1;
#endif
    // Vita user thread priorities must be >= 0x40 (0x40 is highest user priority).
    s_rx_thread = sceKernelCreateThread("cl_voice_rx", rx_worker, rx_prio, rx_stack, 0,
                                        rx_aff, NULL);
    if (s_rx_thread < 0) {
        console_logf("[voice] rx thread create failed 0x%08x", (unsigned)s_rx_thread);
        voice_audio_stop();
        return -1;
    }
    sceKernelStartThread(s_rx_thread, 0, NULL);

    // TX: Opus encoder (mono 16kHz wideband, VOIP-tuned) + mic (VOICE port, 16kHz mono).
    int eerr = 0;
    s_enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &eerr);
    if (s_enc) {
        opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(48000));
        opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(s_enc, OPUS_SET_PACKET_LOSS_PERC(15));
        opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(3));
    } else console_logf("[voice] opus encoder create failed %d", eerr);
    s_denoise = speex_preprocess_state_init(VA_TX_FRAME_SAMPLES, 16000);
    if (s_denoise) {
        int denoise = 1, suppress = -6, agc = 1;
        int agc_inc = 20, agc_max_gain = 35;
        float agc_lvl = 28000.0f;
        speex_preprocess_ctl(s_denoise, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
        speex_preprocess_ctl(s_denoise, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);
        speex_preprocess_ctl(s_denoise, SPEEX_PREPROCESS_SET_AGC, &agc);
        speex_preprocess_ctl(s_denoise, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agc_lvl);
        speex_preprocess_ctl(s_denoise, SPEEX_PREPROCESS_SET_AGC_INCREMENT, &agc_inc);
        speex_preprocess_ctl(s_denoise, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &agc_max_gain);
    }
    s_mic = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, VA_MIC_GRAIN, 16000,
                               SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (s_mic < 0) console_logf("[voice] mic open failed 0x%08x", (unsigned)s_mic);
#ifdef CORDLESS_PLUGIN
    const SceSize tx_stack = 0x10000;
    const int tx_prio = 0x62;
    const int tx_aff = 0;
#else
    const SceSize tx_stack = 0x10000;
    const int tx_prio = 0x42;
    const int tx_aff = SCE_KERNEL_CPU_MASK_USER_2;
#endif
    s_tx_thread = sceKernelCreateThread("cl_voice_tx", tx_worker, tx_prio, tx_stack, 0,
                                        tx_aff, NULL);
    if (s_tx_thread >= 0) sceKernelStartThread(s_tx_thread, 0, NULL);
    else console_logf("[voice] tx thread create failed 0x%08x", (unsigned)s_tx_thread);

    console_dbgf("[voice] audio started (mode=%d out=%d mic=%d)", s_mode_id, s_aport, s_mic);
    return 0;
}

void voice_audio_stop(void)
{
    s_run = 0;
    s_tx_speaking = 0;
    if (s_tx_thread >= 0) {
        sceKernelWaitThreadEnd(s_tx_thread, NULL, NULL);
        sceKernelDeleteThread(s_tx_thread);
        s_tx_thread = -1;
    }
    if (s_rx_thread >= 0) {
        sceKernelWaitThreadEnd(s_rx_thread, NULL, NULL);
        sceKernelDeleteThread(s_rx_thread);
        s_rx_thread = -1;
    }
    if (s_mic >= 0) { sceAudioInReleasePort(s_mic); s_mic = -1; }
    if (s_denoise) { speex_preprocess_state_destroy(s_denoise); s_denoise = NULL; }
    if (s_enc) { opus_encoder_destroy(s_enc); s_enc = NULL; }
    if (s_spk) {
        for (int i = 0; i < VA_MAX_SPEAKERS; i++)
            if (s_spk[i].dec) { opus_decoder_destroy(s_spk[i].dec); s_spk[i].dec = NULL; }
        free(s_spk);
        s_spk = NULL;
    }
    if (s_aport >= 0) { sceAudioOutReleasePort(s_aport); s_aport = -1; }
    s_udp = -1;
}
