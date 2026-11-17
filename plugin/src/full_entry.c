#include "../kernel/cordless_mem.h"
#include "voice/voice.h"
#include "voice/voice_audio.h"
#include "voice/voice_crypto.h"

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <jansson.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define IPC_PORT 47321

extern void console_logf(const char *fmt, ...);
extern void pool_alloc_shutdown(void);
extern void _init_vita_reent(void);
extern void _init_vita_io(void);
extern void _free_vita_io(void);
extern void __libc_init_array(void);
extern void __libc_fini_array(void);
extern int vitasdk_delete_thread_reent(SceUID thid);
extern void main_gw_set_auth(const char *token, const char *cacert, const char *user_id);
extern void main_gw_tick(void);
extern void main_gw_stop(void);

static volatile int s_run = 0;
static SceUID s_thread = -1;
static SceUID s_gw_thread = -1;
static int s_listen_fd = -1;

static void open_ipc_listener(void) {
    if (s_listen_fd >= 0) return;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(IPC_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 4) == 0) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        s_listen_fd = fd;
        console_logf("[full] IPC listening on 127.0.0.1:%d", IPC_PORT);
    } else {
        console_logf("[full] IPC bind/listen failed (errno=%d)", errno);
        close(fd);
    }
}

static int main_gw_worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (s_run) {
        main_gw_tick();
        sceKernelDelayThread(20 * 1000);
    }
    main_gw_stop();
    main_gw_tick();
    vitasdk_delete_thread_reent(0);
    return sceKernelExitDeleteThread(0);
}

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
        const char *user_id  = json_string_value(json_object_get(req, "user_id"));
        const char *token    = json_string_value(json_object_get(req, "token"));
        const char *cacert   = json_string_value(json_object_get(req, "cacert"));
        main_gw_set_auth(token, cacert, user_id);
        int rc = voice_join(guild_id, chan_id, token, cacert);
        if (rc == 0) {
            const char *cname = json_string_value(json_object_get(req, "chan_name"));
            voice_set_channel_meta(cname);
            json_t *peers = json_object_get(req, "peers");
            if (json_is_array(peers)) {
                size_t np = json_array_size(peers);
                for (size_t i = 0; i < np; i++) {
                    json_t *it = json_array_get(peers, i);
                    const char *pid = json_string_value(json_object_get(it, "id"));
                    const char *pnm = json_string_value(json_object_get(it, "name"));
                    if (pid && pnm) voice_note_user_name(pid, pnm);
                }
            }
        }
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
    _init_vita_reent();
    _init_vita_io();
    __libc_init_array();
    console_logf("=== cordless_voice_full started (lazy pool) ===");

    open_ipc_listener();
    s_gw_thread = sceKernelCreateThread("cv_maingw", main_gw_worker, 0x10000100,
                                        0x10000, 0, 0, NULL);
    if (s_gw_thread >= 0) sceKernelStartThread(s_gw_thread, 0, NULL);

    play_ready_chime();

    int combo_held = 0;
    int was_active = 0;
    int reopen_ticks = 0;
    cordless_overlay_state prev_ov;
    memset(&prev_ov, 0xff, sizeof(prev_ov));
    while (s_run) {
        uint32_t act = cordless_mem_take_action();
        if (act && voice_active()) {
            if (act & CORDLESS_ACT_MUTE)  voice_toggle_mute();
            if (act & CORDLESS_ACT_DEAF)  voice_toggle_deaf();
            if (act & CORDLESS_ACT_LEAVE) voice_leave();
        }
        int va_now = voice_active();
        if (was_active && !va_now) main_gw_stop();
        was_active = va_now;
        {
            int a = va_now, c = voice_connected(), m = voice_is_muted(), d = voice_is_deafened();
            int tx = (a && c && voice_audio_is_tx_speaking()) ? 1 : 0;
            cordless_overlay_state ov;
            memset(&ov, 0, sizeof(ov));
            ov.flags = (a  ? CORDLESS_OV_ACTIVE : 0) |
                       (c  ? CORDLESS_OV_CONNECTED : 0) |
                       (m  ? CORDLESS_OV_MUTED : 0) |
                       (d  ? CORDLESS_OV_DEAF : 0) |
                       (tx ? CORDLESS_OV_TX_SPEAKING : 0);
            if (a) {
                voice_hud_peer hp[CORDLESS_HUD_MAX_PEERS];
                int n = voice_get_hud_peers(ov.channel, sizeof(ov.channel), hp, CORDLESS_HUD_MAX_PEERS);
                if (n > CORDLESS_HUD_MAX_PEERS) n = CORDLESS_HUD_MAX_PEERS;
                ov.peer_count = (uint8_t)n;
                for (int i = 0; i < n; i++) {
                    snprintf(ov.peers[i].name, sizeof(ov.peers[i].name), "%.19s", hp[i].name);
                    ov.peers[i].flags = hp[i].speaking ? CORDLESS_PEER_SPEAKING : 0;
                }
            }
            if (memcmp(&ov, &prev_ov, sizeof(ov)) != 0) {
                prev_ov = ov;
                cordless_mem_set_overlay_state(&ov);
            }
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
            for (;;) {
                int cfd = accept(s_listen_fd, NULL, NULL);
                if (cfd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        console_logf("[full] IPC listener reset (errno=%d)", errno);
                        close(s_listen_fd);
                        s_listen_fd = -1;
                        reopen_ticks = 0;
                    }
                    break;
                }
                int cfl = fcntl(cfd, F_GETFL, 0);
                if (cfl >= 0) fcntl(cfd, F_SETFL, cfl & ~O_NONBLOCK);
                struct timeval tv = { 0, 250 * 1000 };
                setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                char buf[2048];
                int tot = 0;
                while (tot < (int)sizeof(buf) - 1) {
                    int n = recv(cfd, buf + tot, sizeof(buf) - 1 - tot, 0);
                    if (n <= 0) break;
                    tot += n;
                    buf[tot] = '\0';
                    if (strchr(buf, '\n')) break;
                }
                if (tot > 0) handle_ipc_line(cfd, buf);
                close(cfd);
            }
        } else if (++reopen_ticks >= 50) {
            reopen_ticks = 0;
            open_ipc_listener();
        }
        sceKernelDelayThread(20 * 1000);
    }

    cordless_mem_set_overlay(0, 0, 0, 0);
    voice_leave();
    if (s_gw_thread >= 0) {
        sceKernelWaitThreadEnd(s_gw_thread, NULL, NULL);
        s_gw_thread = -1;
    } else {
        main_gw_stop();
    }
    if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
    __libc_fini_array();
    _free_vita_io();
    pool_alloc_shutdown();
    console_logf("=== cordless_voice_full stopped ===");
    vitasdk_delete_thread_reent(0);
    return sceKernelExitDeleteThread(0);
}

int main(void) { return 0; }

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;
    s_run = 1;
    s_thread = sceKernelCreateThread("cv_full", full_thread, 0x10000100, 0x8000, 0, 0, NULL);
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
