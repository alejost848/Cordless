#include "media/audio_player.h"
#include "api/discord.h"
#include "console.h"
#include "net_common.h"
#include "tls/https_get.h"
#include "util/b64.h"

#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <opus/opus.h>
#include <speex/speex_preprocess.h>
#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define AP_MAX_BYTES   (4 * 1024 * 1024)
#define AP_GRAIN       960   // 20ms @ 48kHz
#define AP_MAX_OPUS    5760  // 120ms @ 48kHz (max single Opus frame)

static volatile int s_state;       // 0 = idle, 1 = loading, 2 = playing
static volatile int s_stop;
static volatile int s_pos_ms;
static volatile int s_dur_ms;
static char         s_msg_id[24];
static char         s_url[600];
static char         s_cacert[256];
static SceUID       s_thread = -1;

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t rd64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i] << (8 * i));
    return v;
}

// Fast pass over Ogg pages in memory to read the final granule position.
static uint64_t ogg_last_granule(const uint8_t *buf, int len)
{
    uint64_t last = 0;
    int off = 0;
    while (off + 27 <= len) {
        if (memcmp(buf + off, "OggS", 4) != 0) { off++; continue; }
        int nseg = buf[off + 26];
        if (off + 27 + nseg > len) break;
        uint64_t g = rd64le(buf + off + 6);
        if (g != (uint64_t)-1 && g > last) last = g;
        int body = 0;
        for (int i = 0; i < nseg; i++) body += buf[off + 27 + i];
        off += 27 + nseg + body;
    }
    return last;
}

static void play_ogg_opus(const uint8_t *buf, int len)
{
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(48000, 2, &err);
    if (!dec) {
        console_logf("[audio] opus_decoder_create failed %d", err);
        return;
    }
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AP_GRAIN,
                                   48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        console_logf("[audio] sceAudioOutOpenPort failed 0x%08x", (unsigned)port);
        opus_decoder_destroy(dec);
        return;
    }

    // Ping-pong DMA output buffers + staging accumulator for arbitrary Opus frame sizes.
    // Heap-allocated while playing so static .bss stays small.
    int16_t (*out_buf)[AP_GRAIN * 2] = calloc(2, sizeof(*out_buf));
    int16_t *dec_pcm = malloc((size_t)AP_MAX_OPUS * 2 * sizeof(int16_t));
    int16_t *acc     = malloc((size_t)(AP_MAX_OPUS + AP_GRAIN) * 2 * sizeof(int16_t));
    uint8_t *pkt     = malloc(65536);
    if (!out_buf || !dec_pcm || !acc || !pkt) {
        free(out_buf); free(dec_pcm); free(acc); free(pkt);
        sceAudioOutReleasePort(port);
        opus_decoder_destroy(dec);
        return;
    }
    int out_idx = 0, acc_n = 0, pkt_len = 0, pkt_idx = 0;
    int pre_skip = 0;
    uint64_t last_gran = ogg_last_granule(buf, len);
    int64_t played_samples = 0;

    int off = 0;
    while (!s_stop && off + 27 <= len) {
        if (memcmp(buf + off, "OggS", 4) != 0) { off++; continue; }
        int nseg = buf[off + 26];
        if (off + 27 + nseg > len) break;
        const uint8_t *segs = buf + off + 27;
        int data_off = off + 27 + nseg;
        int body = 0;
        for (int i = 0; i < nseg; i++) body += segs[i];
        if (data_off + body > len) break;

        int seg_pos = data_off;
        for (int i = 0; i < nseg && !s_stop; i++) {
            int sl = segs[i];
            if (pkt_len + sl <= 65536) {
                memcpy(pkt + pkt_len, buf + seg_pos, (size_t)sl);
                pkt_len += sl;
            }
            seg_pos += sl;
            if (sl < 255) {
                if (pkt_idx == 0) {
                    if (pkt_len >= 19 && memcmp(pkt, "OpusHead", 8) == 0) {
                        pre_skip = (int)rd16le(pkt + 10);
                        if (last_gran > (uint64_t)pre_skip)
                            s_dur_ms = (int)((last_gran - (uint64_t)pre_skip) / 48);
                    }
                } else if (pkt_idx == 1) {
                    // OpusTags: skip
                } else if (pkt_len > 0) {
                    int got = opus_decode(dec, pkt, pkt_len, dec_pcm, AP_MAX_OPUS, 0);
                    if (got > 0) {
                        s_state = 2;
                        int start = 0;
                        if (pre_skip > 0) {
                            int drop = (pre_skip < got) ? pre_skip : got;
                            start = drop;
                            pre_skip -= drop;
                        }
                        int keep = got - start;
                        if (keep > 0 && acc_n + keep <= AP_MAX_OPUS + AP_GRAIN) {
                            memcpy(acc + acc_n * 2, dec_pcm + start * 2,
                                   (size_t)keep * 2 * sizeof(int16_t));
                            acc_n += keep;
                        }
                        while (!s_stop && acc_n >= AP_GRAIN) {
                            memcpy(out_buf[out_idx], acc, (size_t)AP_GRAIN * 2 * sizeof(int16_t));
                            sceAudioOutOutput(port, out_buf[out_idx]);
                            out_idx ^= 1;
                            played_samples += AP_GRAIN;
                            s_pos_ms = (int)(played_samples / 48);
                            acc_n -= AP_GRAIN;
                            if (acc_n > 0)
                                memmove(acc, acc + AP_GRAIN * 2, (size_t)acc_n * 2 * sizeof(int16_t));
                        }
                    }
                }
                pkt_len = 0;
                pkt_idx++;
            }
        }
        off = data_off + body;
    }

    // Flush any remaining partial grain padded with silence.
    if (!s_stop && acc_n > 0) {
        memset(out_buf[out_idx], 0, sizeof(out_buf[0]));
        memcpy(out_buf[out_idx], acc, (size_t)acc_n * 2 * sizeof(int16_t));
        sceAudioOutOutput(port, out_buf[out_idx]);
        played_samples += acc_n;
        s_pos_ms = (int)(played_samples / 48);
    }

    sceAudioOutReleasePort(port);
    opus_decoder_destroy(dec);
    free(out_buf);
    free(dec_pcm);
    free(acc);
    free(pkt);
}

static int audio_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    unsigned char *data = NULL;
    int len = 0;
    long http = 0;
    int rc = https_get_mbedtls(s_url, s_cacert[0] ? s_cacert : NULL,
                               &data, &len, &http, AP_MAX_BYTES);
    if (!s_stop && rc == 0 && http >= 200 && http < 300 && data && len > 0) {
        play_ogg_opus(data, len);
    } else if (!s_stop) {
        console_logf("[audio] fetch failed rc=%d http=%ld len=%d", rc, http, len);
    }
    free(data);
    s_state = 0;
    s_pos_ms = 0;
    s_dur_ms = 0;
    s_msg_id[0] = '\0';
    return 0;
}

void audio_player_stop(void)
{
    s_stop = 1;
    if (s_thread >= 0) {
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    s_state = 0;
    s_pos_ms = 0;
    s_dur_ms = 0;
    s_msg_id[0] = '\0';
}

void audio_player_toggle(const char *msg_id, const char *url, const char *cacert_path)
{
    if (!msg_id || !msg_id[0] || !url || !url[0]) return;
    bool same = (s_state != 0 && strcmp(s_msg_id, msg_id) == 0);
    audio_player_stop();
    if (same) return;

    snprintf(s_msg_id, sizeof(s_msg_id), "%s", msg_id);
    snprintf(s_url, sizeof(s_url), "%s", url);
    snprintf(s_cacert, sizeof(s_cacert), "%s", cacert_path ? cacert_path : "");
    s_stop = 0;
    s_pos_ms = 0;
    s_dur_ms = 0;
    s_state = 1;

    s_thread = sceKernelCreateThread("cl_audmsg", audio_worker, 0x40, 0x40000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (s_thread >= 0) {
        sceKernelStartThread(s_thread, 0, NULL);
    } else {
        s_state = 0;
        s_msg_id[0] = '\0';
    }
}

int audio_player_status(const char *msg_id, int *pos_ms, int *dur_ms)
{
    if (!msg_id || !msg_id[0] || s_state == 0 || strcmp(s_msg_id, msg_id) != 0)
        return 0;
    if (pos_ms) *pos_ms = s_pos_ms;
    if (dur_ms) *dur_ms = s_dur_ms;
    return s_state;
}

// ---------------------------------------------------------------------------
// Voice message recorder (16kHz mono mic -> Ogg/Opus -> Discord upload)
// ---------------------------------------------------------------------------

#define REC_MIC_GRAIN   256   // 16ms @ 16kHz
#define REC_OPUS_FRAME  320   // 20ms @ 16kHz
#define REC_MAX_MS      60000

static volatile int s_rec_state;    // 0 = idle, 1 = recording, 2 = uploading
static volatile int s_rec_stop;
static volatile int s_rec_send;
static volatile int s_rec_ms;
static volatile int s_rec_level;    // 0..255
static char         s_rec_cid[24];
static char         s_rec_tok[256];
static char         s_rec_ca[256];
static SceUID       s_rec_thread = -1;

static uint32_t ogg_crc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
    }
    return crc;
}

static void wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    uint32_t serial;
    uint32_t seq;
} ogg_buf;

static int ogg_append_page(ogg_buf *o, uint8_t hdr_type, uint64_t gran,
                           const uint8_t *pkt, size_t plen)
{
    size_t nseg = (plen / 255) + 1;
    if (nseg > 255) return -1;
    size_t page_sz = 27 + nseg + plen;
    if (o->len + page_sz > o->cap) {
        size_t ncap = o->cap ? o->cap * 2 : 32768;
        while (ncap < o->len + page_sz) ncap *= 2;
        if (ncap > 1024 * 1024) return -1;
        uint8_t *nb = realloc(o->buf, ncap);
        if (!nb) return -1;
        o->buf = nb;
        o->cap = ncap;
    }
    uint8_t *pg = o->buf + o->len;
    memcpy(pg, "OggS", 4);
    pg[4] = 0;
    pg[5] = hdr_type;
    wr64le(pg + 6, gran);
    wr32le(pg + 14, o->serial);
    wr32le(pg + 18, o->seq++);
    wr32le(pg + 22, 0);
    pg[26] = (uint8_t)nseg;
    size_t rem = plen;
    for (size_t i = 0; i < nseg; i++) {
        uint8_t s = (rem >= 255) ? 255 : (uint8_t)rem;
        pg[27 + i] = s;
        rem -= s;
    }
    if (plen > 0) memcpy(pg + 27 + nseg, pkt, plen);
    wr32le(pg + 22, ogg_crc(pg, page_sz));
    o->len += page_sz;
    return 0;
}

static int rec_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    int mic = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, REC_MIC_GRAIN, 16000,
                                 SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (mic < 0) {
        console_logf("[audio] rec mic open failed 0x%08x", (unsigned)mic);
        s_rec_state = 0;
        return 0;
    }
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc) {
        sceAudioInReleasePort(mic);
        s_rec_state = 0;
        return 0;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));

    SpeexPreprocessState *pp = speex_preprocess_state_init(REC_OPUS_FRAME, 16000);
    if (pp) {
        int denoise = 1, suppress = -20, agc = 1;
        float agc_lvl = 8000.0f;
        speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
        speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);
        speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_AGC, &agc);
        speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agc_lvl);
    }

    ogg_buf ob = { .serial = 0x4352444Cu };
    // OpusHead (19 bytes): 1ch, pre-skip=312 (@ 48kHz), 16000Hz input
    uint8_t head[19] = { 'O','p','u','s','H','e','a','d', 1, 1, 0,0, 0,0,0,0, 0,0, 0 };
    wr16le(head + 10, 312);
    wr32le(head + 12, 16000);
    ogg_append_page(&ob, 0x02, 0, head, sizeof(head));

    // OpusTags (24 bytes): vendor="Cordless"
    uint8_t tags[24] = { 'O','p','u','s','T','a','g','s', 8,0,0,0,
                         'C','o','r','d','l','e','s','s', 0,0,0,0 };
    ogg_append_page(&ob, 0x00, 0, tags, sizeof(tags));

    int16_t in16[REC_MIC_GRAIN];
    int16_t acc[REC_MIC_GRAIN + REC_OPUS_FRAME];
    int acc_n = 0;
    uint8_t opus_pkt[512];
    uint8_t wave_hist[3000];  // 1 byte per 20ms frame (up to 60s)
    int wave_n = 0;
    uint64_t gran = 0;

    while (!s_rec_stop && s_rec_ms < REC_MAX_MS) {
        if (sceAudioInInput(mic, in16) < 0) { sceKernelDelayThread(10 * 1000); continue; }
        if (acc_n + REC_MIC_GRAIN <= (int)(sizeof(acc) / sizeof(acc[0]))) {
            memcpy(acc + acc_n, in16, sizeof(in16));
            acc_n += REC_MIC_GRAIN;
        }
        while (acc_n >= REC_OPUS_FRAME && !s_rec_stop && s_rec_ms < REC_MAX_MS) {
            if (pp) speex_preprocess_run(pp, acc);
            long long e = 0;
            for (int i = 0; i < REC_OPUS_FRAME; i++) {
                int v = acc[i];
                e += (long long)v * v;
            }
            int rms = (int)sqrt((double)e / REC_OPUS_FRAME);
            int lv = (rms * 255) / 8000;
            if (lv > 255) lv = 255;
            s_rec_level = lv;
            if (wave_n < (int)sizeof(wave_hist)) wave_hist[wave_n++] = (uint8_t)lv;

            int n = opus_encode(enc, acc, REC_OPUS_FRAME, opus_pkt, sizeof(opus_pkt));
            if (n > 0) {
                gran += 960;  // 20ms @ 48kHz Ogg Opus clock
                ogg_append_page(&ob, 0x00, gran, opus_pkt, (size_t)n);
            }
            s_rec_ms += 20;
            acc_n -= REC_OPUS_FRAME;
            if (acc_n > 0) memmove(acc, acc + REC_OPUS_FRAME, (size_t)acc_n * sizeof(int16_t));
        }
    }

    sceAudioInReleasePort(mic);
    if (pp) speex_preprocess_state_destroy(pp);
    opus_encoder_destroy(enc);

    if (s_rec_send && ob.len > 0 && wave_n > 0) {
        s_rec_state = 2;
        // Finalize with an empty EOS page if needed so Ogg stream terminates cleanly.
        ogg_append_page(&ob, 0x04, gran, NULL, 0);

        uint8_t wf32[32];
        int nb = wave_n < 32 ? wave_n : 32;
        for (int i = 0; i < nb; i++) wf32[i] = wave_hist[(i * wave_n) / nb];
        char wf_b64[64];
        b64_encode(wf32, (size_t)nb, wf_b64);

        double dur = (double)s_rec_ms / 1000.0;
        int code = discord_send_voice_message(s_rec_cid, ob.buf, (int)ob.len,
                                              dur, wf_b64,
                                              s_rec_tok, s_rec_ca[0] ? s_rec_ca : NULL);
        console_logf("[audio] voice msg upload HTTP %d (%d bytes, %.1fs)", code, (int)ob.len, dur);
    }

    free(ob.buf);
    s_rec_level = 0;
    s_rec_state = 0;
    return 0;
}

int audio_rec_start(void)
{
    if (s_rec_state != 0) return -1;
    audio_player_stop();
    s_rec_stop = 0;
    s_rec_send = 0;
    s_rec_ms = 0;
    s_rec_level = 0;
    s_rec_state = 1;
    s_rec_thread = sceKernelCreateThread("cl_audrec", rec_worker, 0x40, 0x40000, 0,
                                         SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_rec_thread < 0) { s_rec_state = 0; return -1; }
    sceKernelStartThread(s_rec_thread, 0, NULL);
    return 0;
}

void audio_rec_stop(int send, const char *channel_id, const char *token, const char *cacert_path)
{
    if (s_rec_state == 0 && s_rec_thread < 0) return;
    if (send && channel_id && token) {
        snprintf(s_rec_cid, sizeof(s_rec_cid), "%s", channel_id);
        snprintf(s_rec_tok, sizeof(s_rec_tok), "%s", token);
        snprintf(s_rec_ca, sizeof(s_rec_ca), "%s", cacert_path ? cacert_path : "");
        s_rec_send = 1;
    } else {
        s_rec_send = 0;
    }
    s_rec_stop = 1;
    if (s_rec_thread >= 0) {
        sceKernelWaitThreadEnd(s_rec_thread, NULL, NULL);
        sceKernelDeleteThread(s_rec_thread);
        s_rec_thread = -1;
    }
}

int audio_rec_status(int *ms_out, int *level_out)
{
    if (ms_out) *ms_out = s_rec_ms;
    if (level_out) *level_out = s_rec_level;
    return s_rec_state;
}
