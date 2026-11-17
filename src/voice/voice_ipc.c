#include "voice/voice_ipc.h"
#include "model/settings.h"
#include "model/store.h"
#include "console.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <jansson.h>
#include <netinet/in.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PLUGIN_CACERT_PATH "ur0:data/cordless_cacert.pem"

static int      s_avail = -1; // -1 = unknown, 0 = no, 1 = yes
static int      s_active;
static int      s_connected;
static int      s_muted;
static int      s_deafened;
static char     s_guild[24];
static char     s_chan[24];
static char     s_status[32] = "Idle";
static uint64_t s_last_poll_us;

static void ensure_ur0_cacert(const char *src_path) {
    if (!src_path || !src_path[0]) return;
    SceIoStat st;
    if (sceIoGetstat(PLUGIN_CACERT_PATH, &st) >= 0 && st.st_size > 0) return;

    SceUID in = sceIoOpen(src_path, SCE_O_RDONLY, 0);
    if (in < 0) return;
    sceIoMkdir("ur0:data", 0777);
    SceUID out = sceIoOpen(PLUGIN_CACERT_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out >= 0) {
        char buf[4096];
        int r;
        while ((r = sceIoRead(in, buf, sizeof(buf))) > 0) {
            sceIoWrite(out, buf, r);
        }
        sceIoClose(out);
    }
    sceIoClose(in);
}

static void parse_resp(const char *line) {
    if (!line || !line[0]) return;
    json_error_t jerr;
    json_t *r = json_loads(line, 0, &jerr);
    if (!r) return;

    s_active    = (int)json_integer_value(json_object_get(r, "active"));
    s_connected = (int)json_integer_value(json_object_get(r, "connected"));
    s_muted     = (int)json_integer_value(json_object_get(r, "muted"));
    s_deafened  = (int)json_integer_value(json_object_get(r, "deaf"));

    const char *gid = json_string_value(json_object_get(r, "guild_id"));
    const char *cid = json_string_value(json_object_get(r, "channel_id"));
    const char *st  = json_string_value(json_object_get(r, "status"));
    snprintf(s_guild,  sizeof(s_guild),  "%s", gid ? gid : "");
    snprintf(s_chan,   sizeof(s_chan),   "%s", cid ? cid : "");
    snprintf(s_status, sizeof(s_status), "%s", (st && st[0]) ? st : "Idle");

    json_decref(r);
}

#define VOICE_WAKE_PORT 47322

static int ipc_connect(uint16_t port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
            close(fd);
            return -1;
        }
        int so_err = 0;
        socklen_t slen = sizeof(so_err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0 || so_err != 0) {
            close(fd);
            return -1;
        }
    }
    if (fl >= 0) fcntl(fd, F_SETFL, fl); // back to blocking for short RPC
    return fd;
}

static int ipc_rpc_raw(json_t *req) {
    int fd = ipc_connect(VOICE_IPC_PORT, 150);
    if (fd < 0) return -2; // listener unreachable

    char *body = json_dumps(req, JSON_COMPACT);
    if (!body) { close(fd); return -1; }
    size_t blen = strlen(body);
    send(fd, body, blen, 0);
    send(fd, "\n", 1, 0);
    free(body);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { 0, 400 * 1000 };
    if (select(fd + 1, &rfds, NULL, NULL, &tv) > 0) {
        char buf[1024];
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            parse_resp(buf);
        }
    }
    close(fd);
    return 0;
}

static int ipc_probe_status(void) {
    json_t *req = json_object();
    if (!req) return 0;
    json_object_set_new(req, "cmd", json_string("status"));
    int ok = (ipc_rpc_raw(req) == 0);
    json_decref(req);
    return ok;
}

// Wake on-demand cordless_voice_poc loader on localhost:47322 so the user
// doesn't need to press L+R+START manually after boot or sleep.
static int ipc_wake_core(int force_reload) {
    int fd = ipc_connect(VOICE_WAKE_PORT, 120);
    if (fd < 0) return 0;

    const char *msg = force_reload ? "reload\n" : "wake\n";
    send(fd, msg, strlen(msg), 0);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { 3, 0 }; // loading core + starting IPC takes ~100-400ms
    char resp[16];
    memset(resp, 0, sizeof(resp));
    int loaded_ok = 0;
    if (select(fd + 1, &rfds, NULL, NULL, &tv) > 0) {
        ssize_t n = recv(fd, resp, sizeof(resp) - 1, 0);
        if (n > 0 && strncmp(resp, "ok", 2) == 0)
            loaded_ok = 1;
    }
    close(fd);
    console_logf("[voice-ipc] wake(%s) reply: loaded=%d (%s)",
                 force_reload ? "reload" : "wake",
                 loaded_ok, resp[0] ? resp : "timeout");
    return loaded_ok;
}

int voice_ipc_available(void) {
    static int s_wake_tries = 0;
    if (s_avail == 1) return 1;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (s_avail == 0 && (now - s_last_poll_us) < 3000000ULL) return 0;

    int ok = ipc_probe_status();
    if (!ok && s_wake_tries < 2 && ipc_wake_core(s_wake_tries > 0)) {
        s_wake_tries++;
        for (int i = 0; i < 25; i++) {
            sceKernelDelayThread(100 * 1000);
            ok = ipc_probe_status();
            if (ok) break;
        }
        now = sceKernelGetProcessTimeWide();
    }

    if (ok) s_wake_tries = 0;
    s_avail = ok ? 1 : 0;
    s_last_poll_us = now;
    if (s_avail) console_logf("[voice-ipc] background plugin DETECTED");
    return s_avail;
}

static int ipc_rpc(json_t *req) {
    int rc = ipc_rpc_raw(req);
    if (rc == -2) {
        // Listener may have reset across Vita sleep; re-probe/wake once immediately.
        s_avail = -1;
        s_last_poll_us = 0;
        if (voice_ipc_available())
            rc = ipc_rpc_raw(req);
    }
    return (rc == 0) ? 0 : -1;
}

void voice_ipc_poll(void) {
    if (s_avail != 1) return;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - s_last_poll_us < 500000ULL) return;
    s_last_poll_us = now;

    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("status"));
    ipc_rpc(req);
    json_decref(req);
}

int voice_ipc_join(const char *guild_id, const char *channel_id,
                   const char *user_id, const char *token, const char *cacert_path) {
    ensure_ur0_cacert(cacert_path);
    char chan_label[28];
    chan_label[0] = '\0';
    voice_member vms[12];
    int vn = 0;

    store_lock();
    channel *ch = channel_id ? store_find_channel(channel_id) : NULL;
    if (ch && ch->name[0]) {
        if (guild_id && guild_id[0]) snprintf(chan_label, sizeof(chan_label), "#%.26s", ch->name);
        else snprintf(chan_label, sizeof(chan_label), "%.27s", ch->name);
    }
    if (channel_id && channel_id[0]) {
        vn = store_voice_for_channel(channel_id, vms, 12);
    }
    store_unlock();

    json_t *req = json_object();
    json_object_set_new(req, "cmd",        json_string("join"));
    json_object_set_new(req, "guild_id",   json_string(guild_id ? guild_id : ""));
    json_object_set_new(req, "channel_id", json_string(channel_id ? channel_id : ""));
    json_object_set_new(req, "user_id",    json_string(user_id ? user_id : ""));
    json_object_set_new(req, "token",      json_string(token ? token : ""));
    json_object_set_new(req, "cacert",     json_string(PLUGIN_CACERT_PATH));
    json_object_set_new(req, "noise_suppress", json_integer(g_settings.noise_suppress ? 1 : 0));
    if (chan_label[0]) {
        json_object_set_new(req, "chan_name", json_string(chan_label));
    }
    if (vn > 0) {
        json_t *arr = json_array();
        for (int i = 0; i < vn; i++) {
            if (!vms[i].user_id[0] || !vms[i].name[0]) continue;
            char short_name[20];
            snprintf(short_name, sizeof(short_name), "%s", vms[i].name);
            json_t *item = json_object();
            json_object_set_new(item, "id", json_string(vms[i].user_id));
            json_object_set_new(item, "name", json_string(short_name));
            json_array_append_new(arr, item);
        }
        json_object_set_new(req, "peers", arr);
    }
    int rc = ipc_rpc(req);
    json_decref(req);
    return rc;
}

void voice_ipc_leave(void) {
    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("leave"));
    ipc_rpc(req);
    json_decref(req);
}

void voice_ipc_on_server(const char *token, const char *endpoint,
                         const char *session_id, const char *guild_id,
                         const char *channel_id, const char *user_id) {
    json_t *req = json_object();
    json_object_set_new(req, "cmd",        json_string("server"));
    json_object_set_new(req, "token",      json_string(token ? token : ""));
    json_object_set_new(req, "endpoint",   json_string(endpoint ? endpoint : ""));
    json_object_set_new(req, "session_id", json_string(session_id ? session_id : ""));
    json_object_set_new(req, "guild_id",   json_string(guild_id ? guild_id : ""));
    json_object_set_new(req, "channel_id", json_string(channel_id ? channel_id : ""));
    json_object_set_new(req, "user_id",    json_string(user_id ? user_id : ""));
    ipc_rpc(req);
    json_decref(req);
}

void voice_ipc_on_state_update(const char *guild_id, const char *channel_id,
                               const char *session_id) {
    json_t *req = json_object();
    json_object_set_new(req, "cmd",        json_string("state"));
    json_object_set_new(req, "guild_id",   json_string(guild_id ? guild_id : ""));
    json_object_set_new(req, "channel_id", json_string(channel_id ? channel_id : ""));
    json_object_set_new(req, "session_id", json_string(session_id ? session_id : ""));
    ipc_rpc(req);
    json_decref(req);
}

void voice_ipc_set_muted(int muted) {
    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("mute"));
    json_object_set_new(req, "val", json_integer(muted ? 1 : 0));
    ipc_rpc(req);
    json_decref(req);
}

void voice_ipc_set_deafened(int deafened) {
    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("deaf"));
    json_object_set_new(req, "val", json_integer(deafened ? 1 : 0));
    ipc_rpc(req);
    json_decref(req);
}

void voice_ipc_set_noise_suppress(int enabled) {
    if (s_avail != 1) return;
    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("noise"));
    json_object_set_new(req, "val", json_integer(enabled ? 1 : 0));
    ipc_rpc(req);
    json_decref(req);
}

int         voice_ipc_active(void)     { voice_ipc_poll(); return s_active; }
int         voice_ipc_connected(void)  { return s_connected; }
int         voice_ipc_is_muted(void)   { return s_muted; }
int         voice_ipc_is_deafened(void){ return s_deafened; }
const char *voice_ipc_guild_id(void)   { return s_guild; }
const char *voice_ipc_channel_id(void) { return s_chan; }
const char *voice_ipc_status(void)     { return s_status; }
