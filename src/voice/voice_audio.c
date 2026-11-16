#include "voice/voice_audio.h"
#include "voice/voice_crypto.h"
#if VOICE_DAVE_ENABLE
#include "voice/dave_frame.h"
#endif
#include "console.h"
#include "net_common.h"

#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <opus/opus.h>
#include <psp2/audioout.h>
#include <psp2/audioin.h>
#include <psp2/kernel/threadmgr.h>

// Voice audio pipeline (Vita-only: Opus + sceAudio + threads). Transport crypto
// and RTP framing live in voice_crypto.c.

int voice_audio_selftest(void) { return voice_crypto_selftest(); }

// RX thread: drains the non-blocking UDP socket (net_lock held only per recv),
// decrypts + Opus-decodes per SSRC, mixes into 20ms stereo frames for sceAudioOut.

#define VA_FRAME_SAMPLES 960  // 20ms @ 48kHz, per channel
#define VA_MAX_SPEAKERS  8
#define VA_JITTER_FRAMES 4

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
    uint8_t      r_idx;
    uint8_t      w_idx;
    uint8_t      count;
    int16_t      pcm[VA_JITTER_FRAMES][VA_FRAME_SAMPLES * 2];
} spk_slot;
static spk_slot s_spk[VA_MAX_SPEAKERS];

void voice_audio_set_muted(int muted)       { s_muted = muted ? 1 : 0; }
int  voice_audio_is_muted(void)             { return s_muted; }
void voice_audio_set_deafened(int deafened) { s_deafened = deafened ? 1 : 0; }
int  voice_audio_is_deafened(void)          { return s_deafened; }
int  voice_audio_is_tx_speaking(void)       { return (!s_muted && !s_deafened && s_tx_speaking) ? 1 : 0; }

int voice_audio_ssrc_speaking(unsigned ssrc)
{
    if (!ssrc || s_deafened) return 0;
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
    if (!s_deafened) {
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
            if (!d) return NULL;
            memset(&s_spk[i], 0, sizeof(s_spk[i]));
            s_spk[i].dec = d;
            s_spk[i].ssrc = ssrc;
            s_spk[i].last_tick = tick;
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
    return &s_spk[lru];
}

static volatile int s_cue_kind = -1;
static volatile int s_cue_frames_left = 0;

void voice_audio_play_cue(int kind)
{
    s_cue_kind = kind;
    s_cue_frames_left = (kind == 0) ? 6 : 2;  // 120ms connect chime, 40ms toggle blip
}

static void mix_cue_into(int16_t *mix)
{
    int left = s_cue_frames_left;
    if (left <= 0) return;
    int kind = s_cue_kind;
    int total = (kind == 0) ? 6 : 2;
    int frame_idx = total - left;
    float freq = (kind == 0) ? (frame_idx < 3 ? 523.25f : 783.99f) : 659.25f;
    float phase_step = 2.0f * 3.14159265f * freq / 48000.0f;
    static float s_phase = 0.0f;
    for (int i = 0; i < VA_FRAME_SAMPLES; i++) {
        float env = (left == 1 && i > VA_FRAME_SAMPLES / 2)
                  ? (float)(VA_FRAME_SAMPLES - i) / (float)(VA_FRAME_SAMPLES / 2)
                  : 1.0f;
        int16_t s = (int16_t)(sinf(s_phase) * 1400.0f * env);
        s_phase += phase_step;
        if (s_phase > 6.2831853f) s_phase -= 6.2831853f;
        int l = (int)mix[i * 2] + s;
        int r = (int)mix[i * 2 + 1] + s;
        mix[i * 2]     = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
        mix[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }
    s_cue_frames_left = left - 1;
}

static int rx_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    static uint8_t pkt[1600], plain[1600];
    static int16_t mix[VA_FRAME_SAMPLES * 2];

    while (s_run) {
        unsigned tick = ++s_rx_tick;
        for (int guard = 0; guard < 64 && s_run; guard++) {  // drain this tick's packets
            int n = (int)recv(s_udp, pkt, sizeof pkt, 0);  // O_NONBLOCK: returns fast
            if (n <= 0) break;
            if (n < 12) continue;
            int ptype = pkt[1] & 0x7F;
            if (ptype >= 72 && ptype <= 76) continue;  // RTCP, not Opus
            unsigned ssrc = ((unsigned)pkt[8] << 24) | ((unsigned)pkt[9] << 16) |
                            ((unsigned)pkt[10] << 8) | pkt[11];
            if (ssrc == s_ssrc) continue;  // our own loopback
            int plen = voice_packet_open(s_mode_id, s_key, pkt, (size_t)n, plain, sizeof plain);
            if (plen < 0) continue;  // auth fail / malformed
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
                    if (dl <= 0) continue;  // E2EE auth fail or no key: drop
                    opus = dave_plain; olen = dl;
                }
            }
#endif
            if (s_deafened) continue;
            spk_slot *sp = speaker_for(ssrc, tick);
            if (!sp) continue;
            int is_sil = (olen == 3 && opus[0] == 0xF8 && opus[1] == 0xFF && opus[2] == 0xFE);
            if (!is_sil) sp->speaking_until = tick + 12;
            int16_t *dst = sp->pcm[sp->w_idx];
            int got = opus_decode(sp->dec, opus, olen, dst, VA_FRAME_SAMPLES, 0);
            if (got <= 0) continue;
            if (got < VA_FRAME_SAMPLES)
                memset(dst + got * 2, 0, (size_t)(VA_FRAME_SAMPLES - got) * 2 * sizeof(int16_t));
            sp->w_idx = (uint8_t)((sp->w_idx + 1) % VA_JITTER_FRAMES);
            if (sp->count < VA_JITTER_FRAMES)
                sp->count++;
            else
                sp->r_idx = (uint8_t)((sp->r_idx + 1) % VA_JITTER_FRAMES);
        }
        memset(mix, 0, sizeof mix);
        if (s_deafened) {
            for (int i = 0; i < VA_MAX_SPEAKERS; i++) {
                s_spk[i].r_idx = s_spk[i].w_idx = s_spk[i].count = 0;
                s_spk[i].speaking_until = 0;
            }
        } else {
            for (int s = 0; s < VA_MAX_SPEAKERS; s++) {
                spk_slot *sp = &s_spk[s];
                if (!sp->dec || sp->count == 0) continue;
                const int16_t *src = sp->pcm[sp->r_idx];
                sp->r_idx = (uint8_t)((sp->r_idx + 1) % VA_JITTER_FRAMES);
                sp->count--;
                for (int i = 0; i < VA_FRAME_SAMPLES * 2; i++) {
                    int v = (int)mix[i] + (int)src[i];
                    mix[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
                }
            }
        }
        mix_cue_into(mix);
        if (s_aport >= 0) sceAudioOutOutput(s_aport, mix);  // blocks ~20ms -> paces us
        else sceKernelDelayThread(20 * 1000);
    }
    return 0;
}

// TX: Vita mic (16kHz mono) upsampled to 48kHz, framed to 20ms, energy-VAD gated,
// Opus-encoded -> sealed -> UDP. Five 0xF8FFFE silence frames end each utterance.

#define VA_MIC_GRAIN   256  // sceAudioIn VOICE grain @ 16kHz (16ms)
#define VA_VAD_MS2     700000  // default mean-square energy gate (Medium)
#define VA_VAD_HANG    25  // quiet frames before we stop (~500ms)

static volatile int       s_ptt_enabled;
static volatile int       s_ptt_down;
static volatile long long s_vad_thresh = VA_VAD_MS2;

void voice_audio_set_ptt(int enabled, int down)
{
    s_ptt_enabled = enabled ? 1 : 0;
    s_ptt_down    = down ? 1 : 0;
}

void voice_audio_set_vad_sens(int sens_level)
{
    if (sens_level <= 0)      s_vad_thresh = 1600000;  // Low sensitivity (requires louder voice)
    else if (sens_level >= 2) s_vad_thresh = 250000;   // High sensitivity (picks up softer voice)
    else                      s_vad_thresh = VA_VAD_MS2;  // Medium (default)
}

static SceUID       s_tx_thread = -1;
static int          s_mic = -1;
static OpusEncoder *s_enc;

static void tx_send(const uint8_t *payload, int len, uint16_t *seq, uint32_t *ts, uint32_t *ctr)
{
    uint8_t hdr[12], pkt[1400];
#if VOICE_DAVE_ENABLE
    // Inner E2EE: wrap the Opus payload with the DAVE frame codec at our per-
    // generation key before transport seal; passthrough until a key is established.
    uint8_t wrapped[1400];
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
    int16_t in16[VA_MIC_GRAIN];
    static int16_t acc[4096]; int accn = 0;
    uint8_t opus[1024];
    uint16_t seq = 0; uint32_t ts = 0, ctr = 1;
    int speaking = 0, quiet = 0;

    while (s_run) {
        if (s_mic < 0 || !s_enc) { sceKernelDelayThread(20 * 1000); continue; }
        if (sceAudioInInput(s_mic, in16) < 0) { sceKernelDelayThread(10 * 1000); continue; }
        for (int i = 0; i < VA_MIC_GRAIN && accn + 3 <= (int)(sizeof(acc)/sizeof(acc[0])); i++) {
            int a = in16[i], b = (i + 1 < VA_MIC_GRAIN) ? in16[i + 1] : in16[i];
            acc[accn++] = (int16_t)a;  // 16k -> 48k linear
            acc[accn++] = (int16_t)(a + (b - a) / 3);
            acc[accn++] = (int16_t)(a + 2 * (b - a) / 3);
        }
        while (accn >= VA_FRAME_SAMPLES && s_run) {
            long long e = 0;
            for (int i = 0; i < VA_FRAME_SAMPLES; i++) { int v = acc[i]; e += (long long)v * v; }
            int active = 0;
            if (!s_muted && !s_deafened) {
                if (s_ptt_enabled) active = s_ptt_down ? 1 : 0;
                else               active = ((e / VA_FRAME_SAMPLES) > s_vad_thresh);
            }
            if (active || (speaking && quiet < VA_VAD_HANG)) {
                if (!speaking) { speaking = 1; s_tx_speaking = 1; quiet = 0; if (s_on_speaking) s_on_speaking(1); }
                quiet = active ? 0 : quiet + 1;
                int n = opus_encode(s_enc, acc, VA_FRAME_SAMPLES, opus, sizeof opus);
                if (n > 0) tx_send(opus, n, &seq, &ts, &ctr);
            } else if (speaking) {  // trailing quiet -> stop
                tx_silence(&seq, &ts, &ctr);
                speaking = 0; s_tx_speaking = 0; quiet = 0;
                if (s_on_speaking) s_on_speaking(0);
            }
            memmove(acc, acc + VA_FRAME_SAMPLES, (accn - VA_FRAME_SAMPLES) * sizeof(acc[0]));
            accn -= VA_FRAME_SAMPLES;
        }
    }
    if (speaking) { tx_silence(&seq, &ts, &ctr); if (s_on_speaking) s_on_speaking(0); }
    s_tx_speaking = 0;
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
    memset(s_spk, 0, sizeof s_spk);
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
    s_rx_thread = sceKernelCreateThread("cl_voice_rx", rx_worker, 0x40, 0x10000, 0,
                                        SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_rx_thread < 0) { voice_audio_stop(); return -1; }
    sceKernelStartThread(s_rx_thread, 0, NULL);

    // TX: Opus encoder (mono 48k) + mic (VOICE port, 16kHz mono).
    int eerr = 0;
    s_enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &eerr);
    if (s_enc) {
        opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(64000));
        opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(s_enc, OPUS_SET_PACKET_LOSS_PERC(15));
        opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(5));
    } else console_logf("[voice] opus encoder create failed %d", eerr);
    s_mic = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, VA_MIC_GRAIN, 16000,
                               SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (s_mic < 0) console_logf("[voice] mic open failed 0x%08x", (unsigned)s_mic);
    s_tx_thread = sceKernelCreateThread("cl_voice_tx", tx_worker, 0x40, 0x10000, 0,
                                        SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_tx_thread >= 0) sceKernelStartThread(s_tx_thread, 0, NULL);

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
    if (s_enc) { opus_encoder_destroy(s_enc); s_enc = NULL; }
    for (int i = 0; i < VA_MAX_SPEAKERS; i++)
        if (s_spk[i].dec) { opus_decoder_destroy(s_spk[i].dec); s_spk[i].dec = NULL; }
    if (s_aport >= 0) { sceAudioOutReleasePort(s_aport); s_aport = -1; }
    s_udp = -1;
}
