#include "../kernel/cordless_mem.h"

#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdarg.h>
#include <stdint.h>

#define LOG_PATH "ur0:data/cordless_voice_poc.log"
#define MIC_GRAIN 256                 /* 16ms @ 16kHz mono */
#define OUT_SAMPLES (MIC_GRAIN * 3)   /* 768 samples = 16ms @ 48kHz stereo */

static volatile int s_running = 0;
static SceUID s_thread_id = -1;
static SceUID s_pool_uid = -1;

static void poc_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = sceClibVsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (len <= 0) return;
    if (len >= (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 2;
    buf[len++] = '\n';

    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
}

static void log_memory(const char *tag) {
    SceKernelFreeMemorySizeInfo info;
    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    int rc = sceKernelGetFreeMemorySize(&info);
    if (rc == 0) {
        poc_log("[%s] free mem: user=%d KiB, cdram=%d KiB, phycont=%d KiB",
                tag, info.size_user / 1024, info.size_cdram / 1024, info.size_phycont / 1024);
    } else {
        poc_log("[%s] sceKernelGetFreeMemorySize failed: 0x%08x", tag, rc);
    }
}

/* Ping-pong buffer + fade envelope to avoid DMA crackle */
static int16_t s_tone_buf[2][OUT_SAMPLES * 2];

static void play_tone(int aport, int freq_hz, int frames, int amp_pct) {
    if (aport < 0 || freq_hz <= 0 || frames <= 0) return;
    const int period = 48000 / freq_hz;
    const int half = period / 2;
    const int max_amp = (32767 * amp_pct) / 100;
    const int total = frames * OUT_SAMPLES;
    const int fade = OUT_SAMPLES;
    int phase = 0;
    int sample_idx = 0;

    for (int f = 0; f < frames; f++) {
        int16_t *pcm = s_tone_buf[f & 1];
        for (int i = 0; i < OUT_SAMPLES; i++) {
            int pos = phase < half ? phase : (period - phase);
            int val = (pos * 2 * max_amp) / half - max_amp;
            if (sample_idx < fade) {
                val = (val * sample_idx) / fade;
            } else if (sample_idx > total - fade) {
                val = (val * (total - sample_idx)) / fade;
            }
            pcm[i * 2 + 0] = (int16_t)val;
            pcm[i * 2 + 1] = (int16_t)val;
            if (++phase >= period) phase = 0;
            sample_idx++;
        }
        sceAudioOutOutput(aport, pcm);
    }
    sceClibMemset(s_tone_buf[frames & 1], 0, sizeof(s_tone_buf[0]));
    sceAudioOutOutput(aport, s_tone_buf[frames & 1]);
}

static int open_best_audio_out(int *out_type) {
    const int types[] = {
        SCE_AUDIO_OUT_PORT_TYPE_MAIN,
        SCE_AUDIO_OUT_PORT_TYPE_BGM,
        SCE_AUDIO_OUT_PORT_TYPE_VOICE,
    };
    for (int i = 0; i < 3; i++) {
        int port = sceAudioOutOpenPort(types[i], OUT_SAMPLES, 48000,
                                       SCE_AUDIO_OUT_MODE_STEREO);
        if (port >= 0) {
            *out_type = types[i];
            return port;
        }
        poc_log("sceAudioOutOpenPort(type=%d) failed: 0x%08x", types[i], port);
    }
    *out_type = -1;
    return -1;
}

static int poc_thread(SceSize args, void *argp) {
    sceIoMkdir("ur0:data", 0777);
    poc_log("=== cordless_voice_poc (net-only) started ===");
    log_memory("boot");

    sceKernelDelayThread(3 * 1000 * 1000);
    log_memory("post-boot");

    /* Hold 3 MiB USER_SHARED_MAIN_RW pool and verify read/write */
    uint32_t pool_addr = 0, pool_type = 0;
    const uint32_t pool_size = 3 * 1024 * 1024;
    s_pool_uid = cordless_mem_alloc_pool(pool_size, &pool_addr, &pool_type);
    if (s_pool_uid >= 0 && pool_addr != 0) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)pool_addr;
        p[0] = 0xCAFEBABE;
        p[(pool_size / 4) - 1] = 0xDEADBEEF;
        poc_log("[pool 3M] OK uid=0x%08x addr=0x%08x type=0x%08x rw=%08x/%08x",
                s_pool_uid, pool_addr, pool_type, p[0], p[(pool_size / 4) - 1]);
    } else {
        poc_log("[pool 3M] FAILED rc=0x%08x", (uint32_t)s_pool_uid);
    }

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static uint8_t net_mem[16 * 1024];
    SceNetInitParam init_param = {
        .memory = net_mem,
        .size = sizeof(net_mem),
        .flags = 0,
    };
    int net_rc = sceNetInit(&init_param);
    int ctl_rc = sceNetCtlInit();
    int udp_sock = sceNetSocket("poc_udp", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    poc_log("net init=0x%08x, ctl=0x%08x, udp=%d", net_rc, ctl_rc, udp_sock);

    int out_type = -1;
    int aport = open_best_audio_out(&out_type);
    if (aport >= 0) {
        int vols[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        sceAudioOutSetVolume(aport, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vols);
        poc_log("audio out opened: port=%d type=%d", aport, out_type);
        play_tone(aport, 440, 18, 12);
    }

    int mic = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, MIC_GRAIN, 16000,
                                 SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    poc_log("audio in opened: port=%d", mic);

    int16_t in16[MIC_GRAIN];
    int16_t out48[2][OUT_SAMPLES * 2];
    int out_idx = 0;
    int loopback = 0;
    int combo_held = 0;
    int iter = 0;
    int16_t peak = 0;

    while (s_running) {
        SceCtrlData pad;
        sceClibMemset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);

        const uint32_t combo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
        if ((pad.buttons & combo) == combo) {
            if (!combo_held) {
                combo_held = 1;
                loopback = !loopback;
                poc_log("combo L+R+SELECT -> loopback=%d", loopback);
                play_tone(aport, loopback ? 660 : 330, 8, 10);
            }
        } else {
            combo_held = 0;
        }

        if (mic >= 0 && sceAudioInInput(mic, in16) >= 0) {
            int16_t *dst = out48[out_idx & 1];
            for (int i = 0; i < MIC_GRAIN; i++) {
                int16_t v = in16[i];
                int16_t av = v < 0 ? (int16_t)-v : v;
                if (av > peak) peak = av;
                if (loopback && aport >= 0) {
                    int16_t atten = (int16_t)(v / 2);
                    for (int k = 0; k < 3; k++) {
                        int idx = (i * 3 + k) * 2;
                        dst[idx + 0] = atten;
                        dst[idx + 1] = atten;
                    }
                }
            }
            if (loopback && aport >= 0) {
                sceAudioOutOutput(aport, dst);
                out_idx++;
            }
        } else {
            sceKernelDelayThread(16 * 1000);
        }

        if (++iter >= 312) {
            iter = 0;
            int net_state = -1;
            sceNetCtlInetGetState(&net_state);
            poc_log("[tick] net_state=%d mic_peak=%d loopback=%d",
                    net_state, (int)peak, loopback);
            log_memory("tick");
            peak = 0;
        }
    }

    if (mic >= 0) sceAudioInReleasePort(mic);
    if (aport >= 0) sceAudioOutReleasePort(aport);
    if (udp_sock >= 0) sceNetSocketClose(udp_sock);
    if (s_pool_uid >= 0) {
        cordless_mem_free_pool(s_pool_uid);
        s_pool_uid = -1;
    }
    poc_log("=== cordless_voice_poc stopped ===");
    return sceKernelExitDeleteThread(0);
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    s_running = 1;
    s_thread_id = sceKernelCreateThread("cordless_poc_thr", poc_thread,
                                      0x10000100, 0x4000, 0, 0, NULL);
    if (s_thread_id >= 0) {
        sceKernelStartThread(s_thread_id, 0, NULL);
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    s_running = 0;
    if (s_thread_id >= 0) {
        sceKernelWaitThreadEnd(s_thread_id, NULL, NULL);
        s_thread_id = -1;
    }
    return SCE_KERNEL_STOP_SUCCESS;
}
