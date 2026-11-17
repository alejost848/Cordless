#include "tls/ws_client.h"
#include "voice/voice.h"

#include <jansson.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void console_logf(const char *fmt, ...);

static ws_client *s_ws = NULL;
static char   *s_buf = NULL;
static size_t  s_cap = 0;
static int     s_hb_ms = 41250;
static int64_t s_last_hb = 0;
static int64_t s_last_rx = 0;
static int64_t s_last_retry = 0;
static int     s_seq = -1;
static int     s_ready = 0;
static int     s_identified = 0;
static volatile int s_active = 0;
static volatile int s_stop_req = 0;

static SceUID s_gw_mtx = -1;
static void gw_lock(void) {
    if (s_gw_mtx < 0) {
        SceUID m = sceKernelCreateMutex("cmgw", 0, 0, NULL);
        if (m >= 0) {
            if (!__sync_bool_compare_and_swap(&s_gw_mtx, -1, m))
                sceKernelDeleteMutex(m);
        }
    }
    if (s_gw_mtx >= 0) sceKernelLockMutex(s_gw_mtx, 1, NULL);
}
static void gw_unlock(void) {
    if (s_gw_mtx >= 0) sceKernelUnlockMutex(s_gw_mtx, 1);
}

static char s_token[160];
static char s_cacert[128];

static char s_guild[24];
static char s_chan[24];
static int  s_mute;
static int  s_deaf;
static int  s_dirty = 0;

static char s_me_id[24];
static char s_sess_id[64];

/* Brace-depth aware streaming JSON scanner for large READY frames (0 extra heap) */
typedef enum {
    ST_SEEK_KEY = 0,
    ST_IN_KEY,
    ST_AFTER_KEY,
    ST_VAL_STR,
    ST_VAL_SKIP,
} scan_state_t;

static struct {
    int depth;          /* { } nesting depth (1 = root object, 2 = root "d") */
    int arr_depth;      /* [ ] nesting depth */
    int esc;
    int in_str;
    scan_state_t st;
    char key[32];
    size_t klen;
    char val[80];
    size_t vlen;
    int capture;        /* 1 = "t", 2 = "session_id" (depth==2, arr_depth==0), 3 = "id" */
    int is_ready;
    int got_sid;
    int got_uid;
} s_sc;

static int64_t gw_now_ms(void) {
    SceRtcTick t;
    sceRtcGetCurrentTick(&t);
    return (int64_t)(t.tick / 1000);
}

void main_gw_set_auth(const char *token, const char *cacert, const char *user_id) {
    gw_lock();
    if (token && strcmp(s_token, token) != 0)
        s_sess_id[0] = '\0';
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    snprintf(s_cacert, sizeof(s_cacert), "%s",
             (cacert && cacert[0]) ? cacert : "ur0:data/cordless_cacert.pem");
    if (user_id && user_id[0]) {
        snprintf(s_me_id, sizeof(s_me_id), "%s", user_id);
    }
    gw_unlock();
}

static void scan_reset(void) {
    memset(&s_sc, 0, sizeof(s_sc));
}

static void on_large_gw_chunk(const unsigned char *chunk, size_t len, int done, void *ctx) {
    (void)ctx;
    if (done) {
        if (s_sc.is_ready && (s_sess_id[0] || s_me_id[0])) {
            s_ready = 1;
            if (s_seq < 0) s_seq = 1;
            console_logf("[maingw] READY (streamed) uid=%s sid=%s",
                         s_me_id, s_sess_id[0] ? "ok" : "none");
        }
        scan_reset();
        return;
    }

    for (size_t i = 0; i < len; i++) {
        char c = (char)chunk[i];

        if (s_sc.st == ST_IN_KEY) {
            if (s_sc.esc) { s_sc.esc = 0; continue; }
            if (c == '\\') { s_sc.esc = 1; continue; }
            if (c == '"') {
                s_sc.key[s_sc.klen] = '\0';
                s_sc.st = ST_AFTER_KEY;
            } else if (s_sc.klen + 1 < sizeof(s_sc.key)) {
                s_sc.key[s_sc.klen++] = c;
            }
            continue;
        }

        if (s_sc.st == ST_VAL_STR) {
            if (s_sc.esc) { s_sc.esc = 0; continue; }
            if (c == '\\') { s_sc.esc = 1; continue; }
            if (c == '"') {
                s_sc.val[s_sc.vlen] = '\0';
                if (s_sc.capture == 1 && strcmp(s_sc.val, "READY") == 0) {
                    s_sc.is_ready = 1;
                } else if (s_sc.capture == 2 && !s_sc.got_sid && s_sc.val[0]) {
                    snprintf(s_sess_id, sizeof(s_sess_id), "%.63s", s_sc.val);
                    s_sc.got_sid = 1;
                } else if (s_sc.capture == 3 && !s_sc.got_uid && !s_me_id[0] && s_sc.val[0]) {
                    snprintf(s_me_id, sizeof(s_me_id), "%.23s", s_sc.val);
                    s_sc.got_uid = 1;
                }
                s_sc.capture = 0;
                s_sc.st = ST_SEEK_KEY;
            } else if (s_sc.capture && s_sc.vlen + 1 < sizeof(s_sc.val)) {
                s_sc.val[s_sc.vlen++] = c;
            }
            continue;
        }

        if (s_sc.st == ST_VAL_SKIP) {
            if (s_sc.in_str) {
                if (s_sc.esc) s_sc.esc = 0;
                else if (c == '\\') s_sc.esc = 1;
                else if (c == '"') s_sc.in_str = 0;
                continue;
            }
            if (c == '"') { s_sc.in_str = 1; continue; }
            if (c == '{') s_sc.depth++;
            else if (c == '}') { if (s_sc.depth > 0) s_sc.depth--; }
            else if (c == '[') s_sc.arr_depth++;
            else if (c == ']') { if (s_sc.arr_depth > 0) s_sc.arr_depth--; }
            if (c == ',' || c == '}' || c == ']') s_sc.st = ST_SEEK_KEY;
            continue;
        }

        if (s_sc.st == ST_AFTER_KEY) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':') continue;
            if (c == '"') {
                s_sc.capture = 0;
                s_sc.vlen = 0;
                if (s_sc.depth == 1 && s_sc.arr_depth == 0 && strcmp(s_sc.key, "t") == 0)
                    s_sc.capture = 1;
                else if (s_sc.depth == 2 && s_sc.arr_depth == 0 && strcmp(s_sc.key, "session_id") == 0)
                    s_sc.capture = 2;
                else if (s_sc.depth == 3 && s_sc.arr_depth == 0 && strcmp(s_sc.key, "id") == 0)
                    s_sc.capture = 3;
                s_sc.st = ST_VAL_STR;
            } else if (c == '{') {
                s_sc.depth++;
                s_sc.st = ST_SEEK_KEY;
            } else if (c == '[') {
                s_sc.arr_depth++;
                s_sc.st = ST_SEEK_KEY;
            } else {
                s_sc.st = ST_VAL_SKIP;
            }
            continue;
        }

        /* ST_SEEK_KEY */
        if (c == '{') s_sc.depth++;
        else if (c == '}') { if (s_sc.depth > 0) s_sc.depth--; }
        else if (c == '[') s_sc.arr_depth++;
        else if (c == ']') { if (s_sc.arr_depth > 0) s_sc.arr_depth--; }
        else if (c == '"') {
            if (s_sc.arr_depth == 0 && s_sc.depth <= 3) {
                s_sc.klen = 0;
                s_sc.st = ST_IN_KEY;
            } else {
                s_sc.in_str = 1;
                s_sc.st = ST_VAL_SKIP;
            }
        }
    }
}

static void send_op4_vals(ws_client *ws, const char *guild, const char *chan, int mute, int deaf) {
    json_t *d = json_object();
    if (guild[0]) json_object_set_new(d, "guild_id", json_string(guild));
    else          json_object_set_new(d, "guild_id", json_null());
    if (chan[0])  json_object_set_new(d, "channel_id", json_string(chan));
    else          json_object_set_new(d, "channel_id", json_null());
    json_object_set_new(d, "self_mute", json_boolean(mute));
    json_object_set_new(d, "self_deaf", json_boolean(deaf));

    json_t *pkt = json_pack("{si,so}", "op", 4, "d", d);
    char *txt = json_dumps(pkt, JSON_COMPACT);
    json_decref(pkt);
    if (txt) {
        int rc = ws_send_text(ws, txt, (int)strlen(txt));
        console_logf("[maingw] OP4 sent chan=%s rc=%d", chan[0] ? chan : "(null)", rc);
        free(txt);
    }
}

static void send_identify_tok(ws_client *ws, const char *tok) {
    json_t *props = json_pack("{ss,ss,ss}",
                              "os", "Windows",
                              "browser", "Discord Client",
                              "device", "Cordless");
    json_t *cs = json_pack("{s:{},s:s,s:i,s:i,s:i,s:s,s:i}",
                           "guild_versions",
                           "highest_last_message_id", "0",
                           "read_state_version", 0,
                           "user_guild_settings_version", -1,
                           "user_settings_version", -1,
                           "private_channels_version", "0",
                           "api_code_version", 0);
    json_t *d = json_pack("{ss,so,sb,si,so}",
                          "token", tok,
                          "properties", props,
                          "compress", 0,
                          "capabilities", 30717,
                          "client_state", cs);
    json_t *pkt = json_pack("{si,so}", "op", 2, "d", d);
    char *txt = json_dumps(pkt, JSON_COMPACT);
    json_decref(pkt);
    if (txt) {
        int rc = ws_send_text(ws, txt, (int)strlen(txt));
        console_logf("[maingw] IDENTIFY sent rc=%d", rc);
        free(txt);
    }
}

static void send_heartbeat(ws_client *ws, int seq) {
    json_t *pkt = json_pack("{si,so}", "op", 1,
                            "d", seq >= 0 ? json_integer(seq) : json_null());
    char *txt = json_dumps(pkt, JSON_COMPACT);
    json_decref(pkt);
    if (txt) {
        ws_send_text(ws, txt, (int)strlen(txt));
        free(txt);
    }
}

static void close_ws(void) {
    if (s_ws) {
        ws_close(s_ws);
        s_ws = NULL;
    }
    free(s_buf);
    s_buf = NULL;
    s_cap = 0;
    s_ready = 0;
    s_identified = 0;
    s_seq = -1;
    scan_reset();
}

void main_gw_tick(void) {
    char tok[160], cacert[128];
    int active, stop_req;
    gw_lock();
    active = s_active;
    stop_req = s_stop_req;
    if (stop_req) s_stop_req = 0;
    snprintf(tok, sizeof(tok), "%s", s_token);
    snprintf(cacert, sizeof(cacert), "%s", s_cacert);
    gw_unlock();

    if (!active || stop_req) {
        if (s_ws) {
            if (s_ready) {
                char g[24], c[24];
                int d = 0, m = 0, df = 0;
                gw_lock();
                if (s_dirty && !s_chan[0]) {
                    d = 1; s_dirty = 0;
                    snprintf(g, sizeof(g), "%s", s_guild);
                    snprintf(c, sizeof(c), "%s", s_chan);
                    m = s_mute; df = s_deaf;
                }
                gw_unlock();
                if (d) send_op4_vals(s_ws, g, c, m, df);
            }
            close_ws();
        }
        s_last_retry = 0;
        s_sess_id[0] = '\0';
        return;
    }
    if (!tok[0]) return;

    int64_t now = gw_now_ms();
    if (!s_ws) {
        if (s_last_retry != 0 && now - s_last_retry < 3000) return;
        s_last_retry = now;
        console_logf("[maingw] connecting...");
        s_ws = ws_open("wss://gateway.discord.gg/?v=10&encoding=json",
                       cacert, "https://discord.com", NULL);
        if (!s_ws) {
            console_logf("[maingw] ws_open failed");
            return;
        }
        ws_set_large_cb(s_ws, 16384, on_large_gw_chunk, NULL);
        s_hb_ms = 41250;
        s_last_hb = now;
        s_last_rx = now;
        s_seq = -1;
        s_ready = 0;
        s_identified = 0;
        scan_reset();
    }

    for (int step = 0; step < 6 && s_ws; step++) {
        int is_bin = 0;
        int n = ws_recv_ex(s_ws, &s_buf, &s_cap, &is_bin);
        if (n < 0) {
            console_logf("[maingw] ws closed (%d)", ws_last_close_code(s_ws));
            close_ws();
            return;
        }
        if (n == 0) break;
        s_last_rx = now;
        if (is_bin) continue;

        json_error_t jerr;
        json_t *root = json_loads(s_buf, 0, &jerr);
        if (!root) continue;

        int op = (int)json_integer_value(json_object_get(root, "op"));
        json_t *js = json_object_get(root, "s");
        if (json_is_integer(js)) s_seq = (int)json_integer_value(js);
        json_t *d = json_object_get(root, "d");

        if (op == 10 && d) {
            int iv = (int)json_integer_value(json_object_get(d, "heartbeat_interval"));
            if (iv > 1000) s_hb_ms = iv;
            console_logf("[maingw] HELLO hb=%d", s_hb_ms);
            s_last_hb = now;
            if (!s_identified) {
                s_identified = 1;
                send_identify_tok(s_ws, tok);
            }
        } else if (op == 1) {
            send_heartbeat(s_ws, s_seq);
            s_last_hb = now;
        } else if (op == 7 || op == 9) {
            console_logf("[maingw] op%d reconnect requested", op);
            json_decref(root);
            close_ws();
            return;
        } else if (op == 0 && d) {
            const char *t = json_string_value(json_object_get(root, "t"));
            if (t && strcmp(t, "READY") == 0) {
                const char *sid = json_string_value(json_object_get(d, "session_id"));
                const char *uid = json_string_value(json_object_get(json_object_get(d, "user"), "id"));
                if (sid) snprintf(s_sess_id, sizeof(s_sess_id), "%s", sid);
                if (uid) snprintf(s_me_id, sizeof(s_me_id), "%s", uid);
                s_ready = 1;
                console_logf("[maingw] READY uid=%s", s_me_id);
            } else if (t && strcmp(t, "VOICE_STATE_UPDATE") == 0) {
                const char *uid = json_string_value(json_object_get(d, "user_id"));
                if (uid && (!s_me_id[0] || strcmp(uid, s_me_id) == 0)) {
                    const char *gid = json_string_value(json_object_get(d, "guild_id"));
                    const char *cid = json_string_value(json_object_get(d, "channel_id"));
                    const char *sid = json_string_value(json_object_get(d, "session_id"));
                    if (sid && sid[0]) snprintf(s_sess_id, sizeof(s_sess_id), "%s", sid);
                    console_logf("[maingw] VOICE_STATE_UPDATE chan=%s", cid ? cid : "(null)");
                    voice_on_state_update(gid ? gid : "", cid ? cid : "", s_sess_id);
                }
            } else if (t && strcmp(t, "VOICE_SERVER_UPDATE") == 0) {
                const char *vtok = json_string_value(json_object_get(d, "token"));
                const char *ep  = json_string_value(json_object_get(d, "endpoint"));
                const char *gid = json_string_value(json_object_get(d, "guild_id"));
                const char *cid = json_string_value(json_object_get(d, "channel_id"));
                char def_chan[24];
                gw_lock();
                snprintf(def_chan, sizeof(def_chan), "%s", s_chan);
                gw_unlock();
                console_logf("[maingw] VOICE_SERVER_UPDATE ep=%s", ep ? ep : "(null)");
                voice_on_server(vtok ? vtok : "", ep ? ep : "", s_sess_id,
                                gid ? gid : "", cid ? cid : def_chan, s_me_id);
            }
        }
        json_decref(root);
    }

    if (!s_ws) return;

    if (s_ready) {
        char g[24], c[24];
        int do_send = 0, m = 0, df = 0;
        gw_lock();
        if (s_dirty) {
            do_send = 1; s_dirty = 0;
            snprintf(g, sizeof(g), "%s", s_guild);
            snprintf(c, sizeof(c), "%s", s_chan);
            m = s_mute; df = s_deaf;
            if (!s_chan[0]) s_active = 0;
        }
        gw_unlock();
        if (do_send) {
            send_op4_vals(s_ws, g, c, m, df);
            if (!c[0]) {
                close_ws();
                s_last_retry = 0;
                return;
            }
        }
    }

    if (now - s_last_rx > (int64_t)s_hb_ms * 2 + 15000) {
        console_logf("[maingw] gateway stale after sleep/timeout, reconnecting");
        close_ws();
        s_last_retry = 0;
        return;
    }

    if (now - s_last_hb >= s_hb_ms) {
        send_heartbeat(s_ws, s_seq);
        s_last_hb = now;
    }
}

int gateway_voice_update_ex(const char *guild_id, const char *channel_id,
                            int self_mute, int self_deaf) {
    gw_lock();
    snprintf(s_guild, sizeof(s_guild), "%s", guild_id ? guild_id : "");
    snprintf(s_chan, sizeof(s_chan), "%s", channel_id ? channel_id : "");
    s_mute = self_mute ? 1 : 0;
    s_deaf = self_deaf ? 1 : 0;
    s_dirty = 1;
    if (s_chan[0]) {
        s_active = 1;
        s_stop_req = 0;
    } else if (!s_ws) {
        s_active = 0;
    }
    gw_unlock();
    return 0;
}

void main_gw_stop(void) {
    gw_lock();
    s_active = 0;
    s_stop_req = 1;
    gw_unlock();
}
