#include "../kernel/cordless_mem.h"
#include "voice/voice.h"
#include "voice/voice_crypto.h"

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <jansson.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IPC_PORT 47321

extern void console_logf(const char *fmt, ...);
extern void pool_alloc_shutdown(void);
extern void main_gw_set_auth(const char *token, const char *cacert);
extern void main_gw_tick(void);
extern void main_gw_stop(void);

static volatile int s_run = 0;
static SceUID s_thread = -1;
static int s_listen_fd = -1;

static void play_ready_chime(void) {
    int aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 768, 48000,
                                    SCE_AUDIO_OUT_MODE_STEREO);
    if (aport < 0) return;
    int vols[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(aport, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vols);

    static int16_t buf[2][768 * 2];
    const int period = 48000 / 523; /* C5 chime so it's distinct from PoC 440Hz */
    const int half = period / 2;
    const int max_amp = (32767 * 10) / 100;
    int phase = 0;
    for (int f = 0; f < 10; f++) {
        int16_t *pcm = buf[f & 1];
        for (int i = 0; i < 768; i++) {
            int pos = phase < half ? phase : (period - phase);
            int16_t s = (int16_t)((pos * 2 * max_amp) / half - max_amp);
            pcm[i * 2 + 0] = s;
            pcm[i * 2 + 1] = s;
            if (++phase >= period) phase = 0;
        }
        sceAudioOutOutput(aport, pcm);
    }
    sceClibMemset(buf[0], 0, sizeof(buf[0]));
    sceAudioOutOutput(aport, buf[0]);
    sceAudioOutReleasePort(aport);
}

static void handle_ipc_line(int fd, const char *line) {
    json_error_t jerr;
    json_t *req = json_loads(line, 0, &jerr);
    if (!req) {
        const char *err = "{\"ok\":false,\"err\":\"bad_json\"}\n";
        send(fd, err, strlen(err), 0);
        return;
    }

    const char *cmd = json_string_value(json_object_get(req, "cmd"));
    if (!cmd) cmd = "";

    json_t *resp = json_object();
    json_object_set_new(resp, "ok", json_true());

    if (strcmp(cmd, "join") == 0) {
        const char *guild_id = json_string_value(json_object_get(req, "guild_id"));
        const char *chan_id  = json_string_value(json_object_get(req, "channel_id"));
        const char *token    = json_string_value(json_object_get(req, "token"));
        const char *cacert   = json_string_value(json_object_get(req, "cacert"));
        main_gw_set_auth(token, cacert);
        int rc = voice_join(guild_id, chan_id, token, cacert);
        json_object_set_new(resp, "rc", json_integer(rc));
    } else if (strcmp(cmd, "server") == 0) {
        const char *token   = json_string_value(json_object_get(req, "token"));
        const char *ep      = json_string_value(json_object_get(req, "endpoint"));
        const char *sess    = json_string_value(json_object_get(req, "session_id"));
        const char *guild   = json_string_value(json_object_get(req, "guild_id"));
        const char *chan    = json_string_value(json_object_get(req, "channel_id"));
        const char *uid     = json_string_value(json_object_get(req, "user_id"));
        voice_on_server(token, ep, sess, guild, chan, uid);
    } else if (strcmp(cmd, "state") == 0) {
        const char *guild   = json_string_value(json_object_get(req, "guild_id"));
        const char *chan    = json_string_value(json_object_get(req, "channel_id"));
        const char *sess    = json_string_value(json_object_get(req, "session_id"));
        voice_on_state_update(guild, chan, sess);
    } else if (strcmp(cmd, "leave") == 0) {
        voice_leave();
    } else if (strcmp(cmd, "mute") == 0) {
        json_t *v = json_object_get(req, "val");
        if (json_is_integer(v)) voice_set_muted((int)json_integer_value(v));
        else voice_toggle_mute();
    } else if (strcmp(cmd, "deaf") == 0) {
        json_t *v = json_object_get(req, "val");
        if (json_is_integer(v)) voice_set_deafened((int)json_integer_value(v));
        else voice_toggle_deaf();
    }

    json_object_set_new(resp, "active",     json_integer(voice_active()));
    json_object_set_new(resp, "connected",  json_integer(voice_connected()));
    json_object_set_new(resp, "muted",      json_integer(voice_is_muted()));
    json_object_set_new(resp, "deaf",       json_integer(voice_is_deafened()));
    json_object_set_new(resp, "guild_id",   json_string(voice_guild_id()));
    json_object_set_new(resp, "channel_id", json_string(voice_channel_id()));
    json_object_set_new(resp, "status",     json_string(voice_status()));

    char *out = json_dumps(resp, JSON_COMPACT);
    if (out) {
        send(fd, out, strlen(out), 0);
        send(fd, "\n", 1, 0);
        free(out);
    }
    json_decref(resp);
    json_decref(req);
}

static int full_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    console_logf("=== cordless_voice_full started (lazy pool) ===");

    play_ready_chime();

    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd >= 0) {
        int yes = 1;
        setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(IPC_PORT);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
            listen(s_listen_fd, 4) == 0) {
            console_logf("[full] IPC listening on 127.0.0.1:%d", IPC_PORT);
        } else {
            console_logf("[full] IPC bind/listen failed");
            close(s_listen_fd);
            s_listen_fd = -1;
        }
    }

    int combo_held = 0;
    int prev_ov = -1;
    while (s_run) {
        main_gw_tick();
        if (!voice_active()) main_gw_stop();
        int a = voice_active(), c = voice_connected(), m = voice_is_muted(), d = voice_is_deafened();
        int ov = (a ? 1 : 0) | (c ? 2 : 0) | (m ? 4 : 0) | (d ? 8 : 0);
        if (ov != prev_ov) {
            prev_ov = ov;
            cordless_mem_set_overlay(a, c, m, d);
        }
        /* In-game mute toggle on L + R + SELECT when a call is active */
        SceCtrlData pad;
        sceClibMemset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const uint32_t mute_combo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
        if ((pad.buttons & mute_combo) == mute_combo) {
            if (!combo_held) {
                combo_held = 1;
                if (voice_active()) {
                    voice_toggle_mute();
                    console_logf("[full] L+R+SELECT -> muted=%d", voice_is_muted());
                }
            }
        } else {
            combo_held = 0;
        }

        if (s_listen_fd >= 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(s_listen_fd, &rfds);
            struct timeval tv = { 0, 50 * 1000 };
            if (select(s_listen_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
                int cfd = accept(s_listen_fd, NULL, NULL);
                if (cfd >= 0) {
                    char buf[2048];
                    int n = recv(cfd, buf, sizeof(buf) - 1, 0);
                    if (n > 0) {
                        buf[n] = '\0';
                        handle_ipc_line(cfd, buf);
                    }
                    close(cfd);
                }
            }
        } else {
            sceKernelDelayThread(50 * 1000);
        }
    }

    cordless_mem_set_overlay(0, 0, 0, 0);
    voice_leave();
    main_gw_stop();
    if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
    pool_alloc_shutdown();
    console_logf("=== cordless_voice_full stopped ===");
    return sceKernelExitDeleteThread(0);
}

int main(void) { return 0; }

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;
    s_run = 1;
    s_thread = sceKernelCreateThread("cv_full", full_thread, 0x10000100, 0x6000, 0, 0, NULL);
    if (s_thread >= 0) sceKernelStartThread(s_thread, 0, NULL);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;
    s_run = 0;
    if (s_thread >= 0) {
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        s_thread = -1;
    }
    return SCE_KERNEL_STOP_SUCCESS;
}
