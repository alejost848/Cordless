#include "tls/ws_client.h"
#include "voice/voice.h"

#include <jansson.h>
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
static int64_t s_last_retry = 0;
static int     s_seq = -1;
static int     s_ready = 0;
static int     s_identified = 0;
static int     s_active = 0;

static char s_token[160];
static char s_cacert[128];

static char s_guild[24];
static char s_chan[24];
static int  s_mute;
static int  s_deaf;
static int  s_dirty = 0;

static char s_me_id[24];
static char s_sess_id[64];

/* Rolling overlap buffer for zero-alloc streaming scan of large READY frames */
static char s_scan_tail[512];
static size_t s_scan_tlen = 0;
static int s_scan_got_sid = 0;
static int s_scan_got_uid = 0;
static int s_scan_is_ready = 0;

static int64_t gw_now_ms(void) {
    SceRtcTick t;
    sceRtcGetCurrentTick(&t);
    return (int64_t)(t.tick / 1000);
}

void main_gw_set_auth(const char *token, const char *cacert) {
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    snprintf(s_cacert, sizeof(s_cacert), "%s",
             (cacert && cacert[0]) ? cacert : "ur0:data/cordless_cacert.pem");
}

static void extract_quoted_after(const char *hay, size_t hlen, const char *needle,
                                 char *out, size_t ocap) {
    size_t nlen = strlen(needle);
    if (hlen < nlen + 2) return;
    for (size_t i = 0; i + nlen < hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            size_t s = i + nlen;
            size_t e = s;
            while (e < hlen && hay[e] != '"' && e - s < ocap - 1) e++;
            if (e < hlen && hay[e] == '"' && e > s) {
                memcpy(out, hay + s, e - s);
                out[e - s] = '\0';
                return;
            }
        }
    }
}

static void on_large_gw_chunk(const unsigned char *chunk, size_t len, int done, void *ctx) {
    (void)ctx;
    if (done) {
        if (s_scan_is_ready && (s_sess_id[0] || s_me_id[0])) {
            s_ready = 1;
            if (s_seq < 0) s_seq = 1;
            console_logf("[maingw] READY (streamed) uid=%s", s_me_id);
        }
        s_scan_tlen = 0;
        s_scan_got_sid = 0;
        s_scan_got_uid = 0;
        s_scan_is_ready = 0;
        return;
    }

    size_t off = 0;
    while (off < len) {
        char win[1024];
        size_t take = len - off;
        if (take > sizeof(win) - s_scan_tlen) take = sizeof(win) - s_scan_tlen;
        if (s_scan_tlen > 0) memcpy(win, s_scan_tail, s_scan_tlen);
        memcpy(win + s_scan_tlen, chunk + off, take);
        size_t wlen = s_scan_tlen + take;

        if (!s_scan_is_ready) {
            for (size_t i = 0; i + 11 <= wlen; i++) {
                if (memcmp(win + i, "\"t\":\"READY\"", 11) == 0) {
                    s_scan_is_ready = 1;
                    break;
                }
            }
        }
        if (!s_scan_got_sid) {
            char sid[64] = {0};
            extract_quoted_after(win, wlen, "\"session_id\":\"", sid, sizeof(sid));
            if (sid[0]) {
                snprintf(s_sess_id, sizeof(s_sess_id), "%s", sid);
                s_scan_got_sid = 1;
            }
        }
        if (!s_scan_got_uid) {
            /* Match "user":{"id":"<digits>" or "user":{"username":...,"id":"<digits>" */
            const char *pat = "\"user\":{\"";
            size_t plen = 9;
            for (size_t i = 0; i + plen < wlen; i++) {
                if (memcmp(win + i, pat, plen) == 0) {
                    size_t sub_len = wlen - i;
                    if (sub_len > 256) sub_len = 256;
                    char uid[24] = {0};
                    extract_quoted_after(win + i, sub_len, "\"id\":\"", uid, sizeof(uid));
                    if (uid[0]) {
                        snprintf(s_me_id, sizeof(s_me_id), "%s", uid);
                        s_scan_got_uid = 1;
                        break;
                    }
                }
            }
        }

        size_t keep = wlen > 256 ? 256 : wlen;
        memcpy(s_scan_tail, win + wlen - keep, keep);
        s_scan_tlen = keep;
        off += take;
    }
}

static void send_op4(ws_client *ws) {
    json_t *d = json_object();
    if (s_guild[0]) json_object_set_new(d, "guild_id", json_string(s_guild));
    else            json_object_set_new(d, "guild_id", json_null());
    if (s_chan[0])  json_object_set_new(d, "channel_id", json_string(s_chan));
    else            json_object_set_new(d, "channel_id", json_null());
    json_object_set_new(d, "self_mute", json_boolean(s_mute));
    json_object_set_new(d, "self_deaf", json_boolean(s_deaf));

    json_t *pkt = json_pack("{si,so}", "op", 4, "d", d);
    char *txt = json_dumps(pkt, JSON_COMPACT);
    json_decref(pkt);
    if (txt) {
        ws_send_text(ws, txt, (int)strlen(txt));
        free(txt);
    }
}

static void send_identify(ws_client *ws) {
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
                          "token", s_token,
                          "properties", props,
                          "compress", 0,
                          "capabilities", 30717,
                          "client_state", cs);
    json_t *pkt = json_pack("{si,so}", "op", 2, "d", d);
    char *txt = json_dumps(pkt, JSON_COMPACT);
    json_decref(pkt);
    if (txt) {
        ws_send_text(ws, txt, (int)strlen(txt));
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
    s_scan_tlen = 0;
    s_scan_got_sid = 0;
    s_scan_got_uid = 0;
    s_scan_is_ready = 0;
}

void main_gw_tick(void) {
    if (!s_active) {
        if (s_ws) close_ws();
        return;
    }
    if (!s_token[0]) return;

    int64_t now = gw_now_ms();
    if (!s_ws) {
        if (s_last_retry != 0 && now - s_last_retry < 3000) return;
        s_last_retry = now;
        s_ws = ws_open("wss://gateway.discord.gg/?v=10&encoding=json",
                       s_cacert, "https://discord.com", NULL);
        if (!s_ws) {
            console_logf("[maingw] ws_open failed");
            return;
        }
        ws_set_large_cb(s_ws, 16384, on_large_gw_chunk, NULL);
        s_hb_ms = 41250;
        s_last_hb = now;
        s_seq = -1;
        s_ready = 0;
        s_identified = 0;
        s_scan_tlen = 0;
        s_scan_got_sid = 0;
        s_scan_got_uid = 0;
        s_scan_is_ready = 0;
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
        if (is_bin) continue;

        /* Small frames (< 16 KiB) are parsed with jansson; huge READY went to on_large_gw_chunk */
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
            s_last_hb = now;
            if (!s_identified) {
                s_identified = 1;
                send_identify(s_ws);
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
                if (uid && s_me_id[0] && strcmp(uid, s_me_id) == 0) {
                    const char *gid = json_string_value(json_object_get(d, "guild_id"));
                    const char *cid = json_string_value(json_object_get(d, "channel_id"));
                    const char *sid = json_string_value(json_object_get(d, "session_id"));
                    if (sid && sid[0]) snprintf(s_sess_id, sizeof(s_sess_id), "%s", sid);
                    voice_on_state_update(gid ? gid : "", cid ? cid : "", s_sess_id);
                }
            } else if (t && strcmp(t, "VOICE_SERVER_UPDATE") == 0) {
                const char *tok = json_string_value(json_object_get(d, "token"));
                const char *ep  = json_string_value(json_object_get(d, "endpoint"));
                const char *gid = json_string_value(json_object_get(d, "guild_id"));
                const char *cid = json_string_value(json_object_get(d, "channel_id"));
                voice_on_server(tok ? tok : "", ep ? ep : "", s_sess_id,
                                gid ? gid : "", cid ? cid : s_chan, s_me_id);
            }
        }
        json_decref(root);
    }

    if (!s_ws) return;

    if (s_ready && s_dirty) {
        s_dirty = 0;
        send_op4(s_ws);
        if (!s_chan[0]) {
            s_active = 0;
            close_ws();
            return;
        }
    }

    if (now - s_last_hb >= s_hb_ms) {
        send_heartbeat(s_ws, s_seq);
        s_last_hb = now;
    }
}

int gateway_voice_update_ex(const char *guild_id, const char *channel_id,
                            int self_mute, int self_deaf) {
    snprintf(s_guild, sizeof(s_guild), "%s", guild_id ? guild_id : "");
    snprintf(s_chan, sizeof(s_chan), "%s", channel_id ? channel_id : "");
    s_mute = self_mute ? 1 : 0;
    s_deaf = self_deaf ? 1 : 0;
    s_dirty = 1;
    if (s_chan[0]) s_active = 1;
    else if (!s_ws) s_active = 0;
    return 0;
}

void main_gw_stop(void) {
    s_active = 0;
    close_ws();
}
