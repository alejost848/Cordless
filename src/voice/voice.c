#include "voice/voice.h"
#include "voice/voice_audio.h"
#include "voice/dave.h"
#include "api/discord.h"
#include "gateway/gateway.h"
#include "model/store.h"
#include "tls/ws_client.h"
#include "net_common.h"
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include <jansson.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

// DAVE E2EE is gated by VOICE_DAVE_ENABLE (defined in voice_audio.h). With the
// gate off the client advertises max_dave_protocol_version 0 and creates no MLS session.

enum {
    DAVE_OP_PREPARE_TRANSITION     = 21,  // GW->C JSON  {protocol_version,transition_id}
    DAVE_OP_EXECUTE_TRANSITION     = 22,  // GW->C JSON  {transition_id}
    DAVE_OP_READY_FOR_TRANSITION   = 23,  // C->GW JSON  {transition_id}
    DAVE_OP_PREPARE_EPOCH          = 24,  // GW->C JSON  {protocol_version,epoch}
    DAVE_OP_EXTERNAL_SENDER        = 25,  // GW->C BIN   TLS ExternalSender
    DAVE_OP_KEY_PACKAGE            = 26,  // C->GW BIN   TLS KeyPackage
    DAVE_OP_PROPOSALS              = 27,  // GW->C BIN   TLS proposals -> we commit
    DAVE_OP_COMMIT_WELCOME         = 28,  // C->GW BIN   MLS Commit(+Welcome)
    DAVE_OP_ANNOUNCE_COMMIT        = 29,  // GW->C BIN   winning MLS Commit
    DAVE_OP_WELCOME                = 30,  // GW->C BIN   targeted MLS Welcome
    DAVE_OP_INVALID_COMMIT_WELCOME = 31,  // C->GW JSON  {transition_id}
};

static volatile int s_active;  // join requested, not yet left
static volatile int s_connected;  // session description received
static volatile int s_run;
static volatile int s_muted;
static volatile int s_deafened;
static int          s_pre_deaf_muted;
static const char  *s_status = "Idle";

static char s_guild[24], s_chan[24], s_token[128], s_cacert[256];

// Server assignment from the main gateway (VOICE_SERVER_UPDATE + STATE_UPDATE).
static volatile int s_have_server;
static volatile int s_state_acked;
static char s_vtoken[256], s_endpoint[128], s_session[64], s_user[24];

static unsigned      s_ssrc;
static unsigned char s_secret[32];
static int           s_have_secret;
static char          s_mode[48];
static int           s_udp = -1;
static char          s_local_ip[64];
static int           s_local_port;

static SceUID s_thread = -1;
static ws_client *s_vws;  // voice-gateway ws (op5 Speaking from the TX thread)
static int s_seq_ack = -1;  // last voice gateway seq received (v8 heartbeat)

// ssrc <-> user_id map & Op 5 speaking state, learned from op5 Speaking / op12 Client Connect.
#define VOICE_SSRC_MAP_N 32
static struct { unsigned ssrc; char uid[24]; int speaking; int64_t speaking_at; } s_ssrc_map[VOICE_SSRC_MAP_N];

static int64_t now_ms(void);

static void dave_note_speaker(json_t *d)
{
    if (!d) return;
    unsigned ssrc = (unsigned)json_integer_value(json_object_get(d, "ssrc"));
    if (!ssrc) ssrc = (unsigned)json_integer_value(json_object_get(d, "audio_ssrc"));
    const char *uid = json_string_value(json_object_get(d, "user_id"));
    json_t *spkj = json_object_get(d, "speaking");
    int has_spk = (spkj != NULL);
    int spk = has_spk ? (json_integer_value(spkj) != 0 ? 1 : 0) : 0;
    if (!ssrc && (!uid || !uid[0])) return;

    int slot = -1;
    for (int i = 0; i < VOICE_SSRC_MAP_N; i++) {
        if ((ssrc && s_ssrc_map[i].ssrc == ssrc) ||
            (uid && uid[0] && strcmp(s_ssrc_map[i].uid, uid) == 0)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < VOICE_SSRC_MAP_N; i++) {
            if (!s_ssrc_map[i].ssrc && !s_ssrc_map[i].uid[0]) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) return;
    if (ssrc) s_ssrc_map[slot].ssrc = ssrc;
    if (uid && uid[0]) snprintf(s_ssrc_map[slot].uid, 24, "%s", uid);
    if (has_spk) {
        s_ssrc_map[slot].speaking = spk;
        s_ssrc_map[slot].speaking_at = spk ? now_ms() : 0;
    }
}

static void voice_clear_speaker(const char *uid)
{
    if (!uid || !uid[0]) return;
    for (int i = 0; i < VOICE_SSRC_MAP_N; i++) {
        if (strcmp(s_ssrc_map[i].uid, uid) == 0)
            memset(&s_ssrc_map[i], 0, sizeof(s_ssrc_map[i]));
    }
}

#if VOICE_DAVE_ENABLE
static const char *dave_uid_for_ssrc(unsigned ssrc)
{
    for (int i = 0; i < VOICE_SSRC_MAP_N; i++)
        if (s_ssrc_map[i].ssrc == ssrc) return s_ssrc_map[i].uid;
    return NULL;
}

static dave_session *s_dave;
static int           s_dave_ver;  // negotiated DAVE protocol version (0 = off)
static int           s_dave_pending_tid;  // transition id awaiting op22 execute (-1 = none)
static int           s_dave_pending_ver;  // protocol version for pending transition
static int           s_dave_kp_sent;      // non-zero once initial key package was sent

// Serializes MLS state access between the worker and audio threads.
static SceUID s_dave_mtx = -1;
static void dave_lock(void)   { if (s_dave_mtx >= 0) sceKernelLockMutex(s_dave_mtx, 1, NULL); }
static void dave_unlock(void) { if (s_dave_mtx >= 0) sceKernelUnlockMutex(s_dave_mtx, 1); }

static uint64_t str_to_u64(const char *s)
{
    return s ? (uint64_t)strtoull(s, NULL, 10) : 0;
}
#endif  // VOICE_DAVE_ENABLE

static int64_t now_ms(void)
{
    SceRtcTick t; sceRtcGetCurrentTick(&t);
    return (int64_t)(t.tick / 1000);
}

int         voice_active(void)      { return s_active; }
int         voice_connected(void)   { return s_connected; }
const char *voice_guild_id(void)    { return s_active ? s_guild : ""; }
const char *voice_channel_id(void)  { return s_active ? s_chan : ""; }
const char *voice_status(void)      { return s_status; }
int         voice_is_muted(void)    { return s_muted; }
int         voice_is_deafened(void) { return s_deafened; }

static void voice_sync_self_store(void)
{
    if (!s_active || !s_chan[0]) return;
    store_lock();
    const char *me = store_me_id();
    const char *uid = (me && me[0]) ? me : s_user;
    if (uid && uid[0]) {
        store_voice_set(s_guild, s_chan, uid, NULL, NULL,
                        s_muted ? true : false, s_deafened ? true : false);
        store_unlock();
        store_touch();
    } else {
        store_unlock();
    }
}

void voice_set_muted(int muted)
{
    s_muted = muted ? 1 : 0;
    if (!s_muted && s_deafened) {
        s_deafened = 0;
        voice_audio_set_deafened(0);
    }
    voice_audio_set_muted(s_muted);
    if (s_active) {
        gateway_voice_update_ex(s_guild, s_chan, s_muted, s_deafened);
        voice_sync_self_store();
    }
}

void voice_toggle_mute(void)
{
    voice_set_muted(!s_muted);
    if (s_connected) voice_audio_play_cue(1);
}

void voice_set_deafened(int deafened)
{
    int d = deafened ? 1 : 0;
    if (d) {
        if (!s_deafened) s_pre_deaf_muted = s_muted;
        s_deafened = 1;
        s_muted = 1;
    } else {
        s_deafened = 0;
        s_muted = s_pre_deaf_muted;
    }
    voice_audio_set_deafened(s_deafened);
    voice_audio_set_muted(s_muted);
    if (s_active) {
        gateway_voice_update_ex(s_guild, s_chan, s_muted, s_deafened);
        voice_sync_self_store();
    }
}

void voice_toggle_deaf(void)
{
    voice_set_deafened(!s_deafened);
    if (s_connected) voice_audio_play_cue(1);
}

int voice_is_speaking(const char *user_id)
{
    if (!s_active || !s_connected || !user_id || !user_id[0]) return 0;
    if ((s_user[0] && strcmp(user_id, s_user) == 0) ||
        (store_me_id()[0] && strcmp(user_id, store_me_id()) == 0))
        return (!s_muted && !s_deafened && voice_audio_is_tx_speaking()) ? 1 : 0;
    if (s_deafened) return 0;
    int64_t now = now_ms();
    for (int i = 0; i < VOICE_SSRC_MAP_N; i++) {
        if (s_ssrc_map[i].uid[0] && strcmp(s_ssrc_map[i].uid, user_id) == 0) {
            if (s_ssrc_map[i].ssrc && voice_audio_ssrc_speaking(s_ssrc_map[i].ssrc))
                return 1;
            if (s_ssrc_map[i].speaking) {
                if (now - s_ssrc_map[i].speaking_at <= 1500)
                    return 1;
                s_ssrc_map[i].speaking = 0;
            }
        }
    }
    return 0;
}

unsigned voice_speaking_mask(void)
{
    if (!s_active || !s_connected) return 0;
    unsigned mask = voice_audio_speaking_mask();
    if (!s_deafened) {
        unsigned ws = 0;
        for (int i = 0; i < VOICE_SSRC_MAP_N; i++)
            if (s_ssrc_map[i].speaking) ws |= (1u << (i % 16));
        mask |= (ws << 12);
    }
    return mask;
}

static int vws_send(ws_client *ws, json_t *obj)
{
    char *txt = json_dumps(obj, JSON_COMPACT);
    if (!txt) return -1;
    int r = ws_send_text(ws, txt, -1);
    free(txt);
    return r;
}

static int vws_read(ws_client *ws, char **buf, size_t *cap, int timeout_ms, int *is_binary)
{
    int64_t start = now_ms();
    for (;;) {
        if (!s_run) return -1;
        int r = ws_recv_ex(ws, buf, cap, is_binary);
        if (r > 0) return r;
        if (r < 0) return -1;
        if (now_ms() - start > timeout_ms) return 0;
        sceKernelDelayThread(15 * 1000);
    }
}

// Called from the audio TX thread.
static void voice_send_speaking(int on)
{
    if (!s_vws) return;
    json_t *d = json_pack("{s:i, s:i, s:i}", "speaking", on ? 1 : 0,
                          "delay", 0, "ssrc", (json_int_t)s_ssrc);
    json_t *p = json_pack("{s:i, s:o}", "op", 5, "d", d);
    vws_send(s_vws, p);
    json_decref(p);
}

// Binary framing: GW->C [seq:u16 BE][opcode:u8][payload]; C->GW [opcode:u8][payload].
#if VOICE_DAVE_ENABLE

static void dave_send_binary(ws_client *ws, int op, const uint8_t *payload, size_t len)
{
    uint8_t *frame = malloc(len + 1);
    if (!frame) return;
    frame[0] = (uint8_t)op;
    if (len) memcpy(frame + 1, payload, len);
    int r = ws_send_binary(ws, frame, (int)(len + 1));
    free(frame);
    console_logf("[dave] -> op%d (%u bytes)%s", op, (unsigned)len, r ? " SEND-FAIL" : "");
}

static void dave_send_key_package(ws_client *ws)
{
    if (!s_dave) return;
    uint8_t kp[8192];
    size_t kl = 0;
    if (dave_make_key_package(s_dave, kp, &kl, sizeof(kp)) == 0)
        dave_send_binary(ws, DAVE_OP_KEY_PACKAGE, kp, kl);
    else
        console_logf("[dave] key_package build failed");
}

static void dave_send_ready(ws_client *ws, int tid)
{
    if (tid == 0) return;
    json_t *rd = json_pack("{s:i}", "transition_id", tid);
    json_t *p  = json_pack("{s:i, s:o}", "op", DAVE_OP_READY_FOR_TRANSITION, "d", rd);
    vws_send(ws, p);
    json_decref(p);
}

static void dave_recover_invalid(ws_client *ws, int tid)
{
    console_logf("[dave] invalid commit/welcome tid=%d -> recovering", tid);
    json_t *rd = json_pack("{s:i}", "transition_id", tid);
    json_t *p  = json_pack("{s:i, s:o}", "op", DAVE_OP_INVALID_COMMIT_WELCOME, "d", rd);
    vws_send(ws, p);
    json_decref(p);
    if (s_dave) {
        dave_reset(s_dave);
        voice_audio_invalidate_dave_keys(1);
        if (s_dave_ver >= 1) dave_prepare_epoch(s_dave, 1);
        dave_send_key_package(ws);
        s_dave_kp_sent = 1;
    }
}

// Returns 1 if consumed, 0 to pass the message to the normal handlers.
static int dave_handle_json(ws_client *ws, int op, json_t *d)
{
    switch (op) {
    case DAVE_OP_PREPARE_TRANSITION: {
        int pv  = (int)json_integer_value(json_object_get(d, "protocol_version"));
        int tid = (int)json_integer_value(json_object_get(d, "transition_id"));
        console_logf("[dave] prepare_transition v=%d tid=%d", pv, tid);
        if (tid != 0) {
            s_dave_pending_tid = tid;
            s_dave_pending_ver = pv;
            if (pv == 0) voice_audio_invalidate_dave_keys(0);  // RX passthrough ready
            dave_send_ready(ws, tid);
        } else {
            s_dave_ver = pv;  // tid 0: immediate transition, no execute step
            s_dave_pending_tid = -1;
            s_dave_pending_ver = pv;
            if (pv == 0 && s_dave) dave_reset(s_dave);
            voice_audio_invalidate_dave_keys(1);
        }
        return 1;
    }
    case DAVE_OP_EXECUTE_TRANSITION: {
        int tid = (int)json_integer_value(json_object_get(d, "transition_id"));
        console_logf("[dave] execute_transition tid=%d", tid);
        if (s_dave_pending_tid == tid || tid == 0) {
            s_dave_ver = s_dave_pending_ver;
            if (s_dave_ver == 0 && s_dave) dave_reset(s_dave);
            s_dave_pending_tid = -1;
        }
        voice_audio_invalidate_dave_keys(1);
        return 1;
    }
    case DAVE_OP_PREPARE_EPOCH: {
        int        pv    = (int)json_integer_value(json_object_get(d, "protocol_version"));
        json_int_t epoch = json_integer_value(json_object_get(d, "epoch"));
        console_logf("[dave] prepare_epoch v=%d epoch=%lld", pv, (long long)epoch);
        s_dave_ver = pv;
        if (s_dave && pv >= 1 && epoch == 1) {
            // epoch 1: prepare local group and send a fresh KeyPackage
            dave_prepare_epoch(s_dave, (uint64_t)epoch);
            voice_audio_invalidate_dave_keys(1);
            dave_send_key_package(ws);
            s_dave_kp_sent = 1;
        }
        return 1;
    }
    default:
        return 0;
    }
}

static void dave_handle_binary(ws_client *ws, const uint8_t *msg, size_t n)
{
    if (n < 3) return;
    int            op = msg[2];
    const uint8_t *pl = msg + 3;
    size_t         pn = n - 3;
    console_logf("[dave] <- op%d (%u bytes)", op, (unsigned)pn);
    if (!s_dave) return;
    switch (op) {
    case DAVE_OP_EXTERNAL_SENDER:
        if (dave_on_external_sender(s_dave, pl, pn) != 0)
            console_logf("[dave] external_sender install failed");
        break;
    case DAVE_OP_PROPOSALS: {
        // op28 payload is [Commit][optional Welcome] concatenated
        uint8_t *cbuf = malloc(32768), *wbuf = malloc(32768);
        size_t cl = 0, wl = 0;
        if (cbuf && wbuf &&
            dave_on_proposals(s_dave, pl, pn, NULL, 0, cbuf, &cl, 32768, wbuf, &wl, 32768) == 0) {
            if (cl > 0) {
                uint8_t *both = malloc(cl + wl);
                if (both) {
                    memcpy(both, cbuf, cl);
                    if (wl > 0) memcpy(both + cl, wbuf, wl);
                    dave_send_binary(ws, DAVE_OP_COMMIT_WELCOME, both, cl + wl);
                    free(both);
                }
            }
        } else {
            console_logf("[dave] proposals->commit failed");
        }
        free(cbuf); free(wbuf);
        break;
    }
    case DAVE_OP_ANNOUNCE_COMMIT: {
        if (pn < 2) { console_logf("[dave] announce_commit short"); break; }
        int tid = ((int)pl[0] << 8) | pl[1];
        if (dave_on_commit(s_dave, pl + 2, pn - 2) == 0) {
            s_dave_pending_tid = tid;
            s_dave_pending_ver = s_dave_ver;
            voice_audio_invalidate_dave_keys(tid == 0 ? 1 : 0);
            dave_send_ready(ws, tid);
        } else {
            console_logf("[dave] announce_commit apply failed (tid=%d)", tid);
            dave_recover_invalid(ws, tid);
        }
        break;
    }
    case DAVE_OP_WELCOME: {
        if (pn < 2) { console_logf("[dave] welcome short"); break; }
        int tid = ((int)pl[0] << 8) | pl[1];
        if (dave_on_welcome(s_dave, pl + 2, pn - 2, NULL, 0) == 0) {
            s_dave_pending_tid = tid;
            s_dave_pending_ver = s_dave_ver;
            voice_audio_invalidate_dave_keys(tid == 0 ? 1 : 0);
            dave_send_ready(ws, tid);
        } else {
            console_logf("[dave] welcome join failed (tid=%d)", tid);
            dave_recover_invalid(ws, tid);
        }
        break;
    }
    default:
        console_logf("[dave] unhandled binary op%d", op);
        break;
    }
}

// Audio-thread key resolvers; return 0 until an epoch is established.
static int dave_my_key(uint8_t gen, uint8_t key[16])
{
    int r = 0;
    dave_lock();
    if (s_dave && s_dave_ver >= 1 && dave_established(s_dave))
        r = (dave_sender_key(s_dave, s_user, gen, key) == 0);
    dave_unlock();
    return r;
}

static int dave_peer_key(unsigned ssrc, uint8_t gen, uint8_t key[16])
{
    int r = 0;
    dave_lock();
    if (s_dave && dave_established(s_dave)) {
        const char *uid = dave_uid_for_ssrc(ssrc);
        if (uid) r = (dave_sender_key(s_dave, uid, gen, key) == 0);
    }
    dave_unlock();
    return r;
}

static const voice_dave_hooks s_dave_audio_hooks = { dave_my_key, dave_peer_key };
#endif

static int udp_discover(const char *ip, int port, unsigned ssrc)
{
    s_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_udp < 0) return -1;
    struct sockaddr_in dst; memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET; dst.sin_port = htons((unsigned short)port);
    dst.sin_addr.s_addr = inet_addr(ip);
    if (connect(s_udp, (struct sockaddr *)&dst, sizeof(dst)) < 0) return -1;

    struct timeval tv = { 4, 0 };
    setsockopt(s_udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char pkt[74]; memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x00; pkt[1] = 0x01;  // type: request
    pkt[2] = 0x00; pkt[3] = 70;  // length
    pkt[4] = (ssrc >> 24) & 0xFF; pkt[5] = (ssrc >> 16) & 0xFF;
    pkt[6] = (ssrc >> 8) & 0xFF;  pkt[7] = ssrc & 0xFF;

    int sent = (int)send(s_udp, pkt, sizeof(pkt), 0);
    if (sent != (int)sizeof(pkt)) return -1;

    unsigned char resp[74];
    int got = (int)recv(s_udp, resp, sizeof(resp), 0);
    if (got < 74) { console_logf("[voice] udp discovery no reply (%d)", got); return -1; }

    // address is a NUL-terminated string at offset 8; port is BE16 at offset 72
    resp[71] = '\0';
    snprintf(s_local_ip, sizeof(s_local_ip), "%.63s", (const char *)resp + 8);
    s_local_port = (resp[72] << 8) | resp[73];
    console_dbgf("[voice] discovered %s:%d", s_local_ip, s_local_port);
    return 0;
}

static const char *choose_mode(json_t *modes)
{
    const char *prefer[] = { "aead_xchacha20_poly1305_rtpsize",
                             "aead_aes256_gcm_rtpsize", NULL };
    for (int p = 0; prefer[p]; p++) {
        size_t n = json_array_size(modes);
        for (size_t i = 0; i < n; i++) {
            const char *m = json_string_value(json_array_get(modes, i));
            if (m && strcmp(m, prefer[p]) == 0) return prefer[p];
        }
    }
    const char *first = json_string_value(json_array_get(modes, 0));
    return first ? first : "aead_xchacha20_poly1305_rtpsize";
}


static void send_identify(ws_client *ws)
{
    // advertise our E2EE capability (0 when the gate is off).
    // For DM / Group DM calls Discord expects channel_id as server_id.
    const char *srv_id = s_guild[0] ? s_guild : s_chan;
    int dave_ver = VOICE_DAVE_ENABLE ? DAVE_PROTOCOL_VERSION : 0;
    json_t *d = json_pack("{s:s, s:s, s:s, s:s, s:i}",
                          "server_id", srv_id, "user_id", s_user,
                          "session_id", s_session, "token", s_vtoken,
                          "max_dave_protocol_version", dave_ver);
    json_t *p = json_pack("{s:i, s:o}", "op", 0, "d", d);
    vws_send(ws, p);
    json_decref(p);
    console_dbgf("[voice] IDENTIFY sent (server=%s dave=%d)", srv_id, dave_ver);
}

static void send_select_protocol(ws_client *ws, const char *mode)
{
    snprintf(s_mode, sizeof(s_mode), "%s", mode);
    json_t *data = json_pack("{s:s, s:i, s:s}",
                             "address", s_local_ip, "port", s_local_port, "mode", mode);
    json_t *d = json_pack("{s:s, s:o}", "protocol", "udp", "data", data);
    json_t *p = json_pack("{s:i, s:o}", "op", 1, "d", d);
    vws_send(ws, p);
    json_decref(p);
    console_dbgf("[voice] SELECT PROTOCOL mode=%s", mode);
}

static int worker(SceSize argsz, void *argp)
{
    (void)argsz; (void)argp;

    // wait for the main gateway to deliver the voice server assignment
    int64_t start = now_ms();
    while (s_run && !s_have_server) {
        if (now_ms() - start > 10000) { console_logf("[voice] no server update"); goto done; }
        sceKernelDelayThread(30 * 1000);
    }
    if (!s_run) goto done;

    // For DM / Group DM calls, ring the recipient(s) off-thread (best effort).
    if (!s_guild[0] && s_token[0]) {
        int ring_rc = discord_ring_call(s_chan, s_token, s_cacert);
        console_dbgf("[voice] ring %s -> HTTP %d", s_chan, ring_rc);
    }

    // build a wss URL from the endpoint (may arrive as bare host[:port])
    char url[192];
    snprintf(url, sizeof(url), "wss://%s/?v=8", s_endpoint);

#if VOICE_DAVE_ENABLE
    s_dave_mtx = sceKernelCreateMutex("cl_dave", 0, 0, NULL);
#endif

    for (int attempt = 0; s_run && attempt < 6; attempt++) {
        if (attempt > 0) {
            s_connected = 0;
            s_status = "Reconnecting";
            console_logf("[voice] reconnecting (attempt %d)", attempt);
            for (int w = 0; w < 15 && s_run; w++) sceKernelDelayThread(100 * 1000);
            if (!s_run) break;
        }
        s_status = attempt > 0 ? "Reconnecting" : "Connecting";
        s_seq_ack = -1;
        s_have_secret = 0;
        console_dbgf("[voice] connecting %s", url);

        ws_client *ws = ws_open(url, s_cacert, "https://discord.com", NULL);
        if (!ws) {
            console_logf("[voice] ws connect failed");
            continue;
        }
        s_vws = ws;

#if VOICE_DAVE_ENABLE
        s_dave = NULL; s_dave_ver = 0; s_dave_pending_tid = -1; s_dave_pending_ver = 0;
        s_dave_kp_sent = 0;
        memset(s_ssrc_map, 0, sizeof s_ssrc_map);
        s_dave = dave_session_create(s_user, str_to_u64(s_chan));
        if (!s_dave) console_logf("[dave] session create failed -- E2EE disabled this call");
#endif

        send_identify(ws);

        char *buf = NULL; size_t cap = 0;
        int hb = 0; int64_t last_hb = now_ms();
        int audio_started = 0; unsigned char audio_key[32] = {0};

        while (s_run) {
        int is_bin = 0;
        int n = vws_read(ws, &buf, &cap, 500, &is_bin);
        if (n < 0) { if (s_run) console_logf("[voice] ws closed by server (pre-connect=%d)", !s_connected); break; }
        if (n > 0 && is_bin) {  // binary frame (DAVE only)
            if (n >= 3)
                s_seq_ack = ((int)(uint8_t)buf[0] << 8) | (uint8_t)buf[1];
#if VOICE_DAVE_ENABLE
            dave_lock();
            dave_handle_binary(ws, (const uint8_t *)buf, (size_t)n);
            dave_unlock();
#endif
            // DAVE off: ignore binary frames
        } else if (n > 0) {
            json_error_t e; json_t *root = json_loads(buf, 0, &e);
            if (root) {
                int op = (int)json_integer_value(json_object_get(root, "op"));
                json_t *seqj = json_object_get(root, "seq");
                if (json_is_integer(seqj)) s_seq_ack = (int)json_integer_value(seqj);
                json_t *d = json_object_get(root, "d");
                int dave_consumed = 0;
#if VOICE_DAVE_ENABLE
                if (op >= DAVE_OP_PREPARE_TRANSITION) {
                    dave_lock();
                    dave_consumed = dave_handle_json(ws, op, d);
                    dave_unlock();
                }
#endif
                if (dave_consumed) {
                    // consumed by the DAVE state machine
                } else if (op == 8) {  // Hello
                    hb = (int)json_number_value(json_object_get(d, "heartbeat_interval"));
                    if (hb < 1000) hb = 13750;
                    last_hb = now_ms();
                    console_dbgf("[voice] HELLO hb=%d", hb);
                } else if (op == 2) {  // Ready
                    s_ssrc = (unsigned)json_integer_value(json_object_get(d, "ssrc"));
                    const char *ip = json_string_value(json_object_get(d, "ip"));
                    int port = (int)json_integer_value(json_object_get(d, "port"));
                    json_t *modes = json_object_get(d, "modes");
                    console_dbgf("[voice] READY ssrc=%u %s:%d", s_ssrc, ip ? ip : "?", port);
                    if (ip && udp_discover(ip, port, s_ssrc) == 0)
                        send_select_protocol(ws, choose_mode(modes));
                    else { console_logf("[voice] discovery failed"); json_decref(root); break; }
                } else if (op == 4) {  // Session Description
                    const char *mode = json_string_value(json_object_get(d, "mode"));
                    json_t *key = json_object_get(d, "secret_key");
                    if (json_is_array(key)) {
                        size_t kn = json_array_size(key);
                        for (size_t i = 0; i < kn && i < sizeof(s_secret); i++)
                            s_secret[i] = (unsigned char)json_integer_value(json_array_get(key, i));
                        s_have_secret = 1;
                    }
                    if (mode) snprintf(s_mode, sizeof(s_mode), "%s", mode);
#if VOICE_DAVE_ENABLE
                    json_t *dpv = json_object_get(d, "dave_protocol_version");
                    if (dpv) {
                        s_dave_ver = (int)json_integer_value(dpv);
                        console_logf("[voice] negotiated dave_protocol_version=%d", s_dave_ver);
                        if (s_dave && s_dave_ver >= 1 && !s_dave_kp_sent && !dave_established(s_dave)) {
                            dave_lock();
                            dave_prepare_epoch(s_dave, 1);
                            dave_send_key_package(ws);
                            s_dave_kp_sent = 1;
                            dave_unlock();
                        }
                    }
#endif
                    s_connected = 1; s_status = "Connected";
                    console_dbgf("[voice] SESSION DESCRIPTION mode=%s key=%d bytes -> CONNECTED",
                                 s_mode, s_have_secret ? 32 : 0);
                    // start audio only when the secret_key changes
                    if (s_have_secret && s_udp >= 0 &&
                        (!audio_started || memcmp(audio_key, s_secret, 32) != 0)) {
                        if (audio_started) voice_audio_stop();
#if VOICE_DAVE_ENABLE
                        // register the E2EE key resolvers before the audio threads start
                        voice_audio_set_dave(&s_dave_audio_hooks);
#endif
                        voice_audio_set_muted(s_muted);
                        voice_audio_set_deafened(s_deafened);
                        voice_audio_start(s_udp, s_ssrc, s_secret, s_mode, voice_send_speaking);
                        voice_audio_play_cue(0);
                        memcpy(audio_key, s_secret, 32);
                        audio_started = 1;
                    }
                } else if (op == 5 || op == 12) {  // Speaking / Client Connect: ssrc<->user_id
                    dave_note_speaker(d);
                } else if (op == 13) {  // Client Disconnect
                    const char *duid = json_string_value(json_object_get(d, "user_id"));
                    voice_clear_speaker(duid);
                } else if (op == 6) {  // Heartbeat ACK: ignore
                }
                json_decref(root);
            }
        }
        if (hb > 0 && now_ms() - last_hb >= hb) {
            json_t *hd = json_object();
            json_object_set_new(hd, "t", json_integer((json_int_t)now_ms()));
            json_object_set_new(hd, "seq_ack", json_integer(s_seq_ack));
            json_t *p = json_pack("{s:i, s:o}", "op", 3, "d", hd);
            vws_send(ws, p);
            json_decref(p);
            last_hb = now_ms();
        }
    }

        voice_audio_stop();
        s_vws = NULL;
#if VOICE_DAVE_ENABLE
        voice_audio_set_dave(NULL);
        dave_session_free(s_dave); s_dave = NULL;
#endif
        free(buf);
        ws_close(ws);
        if (s_udp >= 0) { close(s_udp); s_udp = -1; }
    }
    if (s_run && !s_connected) s_status = "Voice failed";

done:
    voice_audio_stop();
    s_vws = NULL;
#if VOICE_DAVE_ENABLE
    voice_audio_set_dave(NULL);
    if (s_dave_mtx >= 0) { sceKernelDeleteMutex(s_dave_mtx); s_dave_mtx = -1; }
    dave_session_free(s_dave); s_dave = NULL;
#endif
    if (s_udp >= 0) { close(s_udp); s_udp = -1; }
    s_connected = 0;
    if (s_active) s_status = "Disconnected";
    return 0;
}


void voice_on_server(const char *token, const char *endpoint,
                     const char *session_id, const char *guild_id,
                     const char *channel_id, const char *user_id)
{
    if (!s_active) return;
    if (s_guild[0]) {
        if (!guild_id || strcmp(guild_id, s_guild) != 0) return;
    } else {
        if (guild_id && guild_id[0]) return;
        if (channel_id && channel_id[0] && strcmp(channel_id, s_chan) != 0) return;
    }
    snprintf(s_vtoken, sizeof(s_vtoken), "%s", token ? token : "");
    snprintf(s_endpoint, sizeof(s_endpoint), "%s", endpoint ? endpoint : "");
    snprintf(s_session, sizeof(s_session), "%s", session_id ? session_id : "");
    snprintf(s_user, sizeof(s_user), "%s", user_id ? user_id : "");
    s_have_server = 1;
    console_dbgf("[voice] server assigned: %s", s_endpoint);
}

void voice_on_state_update(const char *guild_id, const char *channel_id,
                           const char *session_id)
{
    if (!s_active) return;
    if (s_guild[0]) {
        if (!guild_id || strcmp(guild_id, s_guild) != 0) return;
    } else {
        if (guild_id && guild_id[0]) return;
    }
    if (session_id && session_id[0])
        snprintf(s_session, sizeof(s_session), "%s", session_id);
    if (channel_id && channel_id[0]) {
        s_state_acked = 1;
        if (strcmp(s_chan, channel_id) != 0) {
            snprintf(s_chan, sizeof(s_chan), "%s", channel_id);
            console_dbgf("[voice] moved to channel %s", s_chan);
        }
    } else if (s_state_acked || s_connected || s_have_server) {
        console_dbgf("[voice] server disconnected us from voice");
        s_run = 0;
        s_active = 0;
        s_connected = 0;
        s_have_server = 0;
        s_state_acked = 0;
        s_muted = 0;
        s_deafened = 0;
        s_pre_deaf_muted = 0;
        voice_audio_set_muted(0);
        voice_audio_set_deafened(0);
        memset(s_ssrc_map, 0, sizeof(s_ssrc_map));
        s_guild[0] = '\0';
        s_chan[0] = '\0';
        s_status = "Idle";
    }
}

int voice_join(const char *guild_id, const char *channel_id,
               const char *token, const char *cacert_path)
{
    if (!channel_id || !channel_id[0]) return -1;
    if (!VOICE_JOIN_ENABLE) {
        console_logf("[voice] join refused: voice disabled (DAVE E2EE required)");
        return -1;
    }
    if (s_active || s_thread >= 0) voice_leave();

    // run the crypto self-tests once, on first join
    static int s_crypto_checked;
    if (!s_crypto_checked) {
        s_crypto_checked = 1;
        voice_audio_selftest();
#if VOICE_DAVE_ENABLE
        dave_selftest();
#endif
    }

    snprintf(s_guild, sizeof(s_guild), "%s", guild_id ? guild_id : "");
    snprintf(s_chan, sizeof(s_chan), "%s", channel_id);
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    snprintf(s_cacert, sizeof(s_cacert), "%s", cacert_path ? cacert_path : "");
    s_muted = 0;
    s_deafened = 0;
    s_pre_deaf_muted = 0;
    voice_audio_set_muted(0);
    voice_audio_set_deafened(0);
    memset(s_ssrc_map, 0, sizeof(s_ssrc_map));
    s_have_server = 0; s_have_secret = 0; s_connected = 0; s_state_acked = 0;
    s_active = 1; s_run = 1; s_status = "Requesting";

    // ask the main gateway to move us into the voice channel / call (op4)
    gateway_voice_update_ex(s_guild, channel_id, 0, 0);

    s_thread = sceKernelCreateThread("cl_voice", worker, 0x40, 0x40000, 0, 0, NULL);
    if (s_thread < 0) { s_active = 0; s_run = 0; s_status = "Idle"; return -1; }
    sceKernelStartThread(s_thread, 0, NULL);
    console_dbgf("[voice] join %s#%s", s_guild[0] ? s_guild : "(dm)", channel_id);
    return 0;
}

void voice_leave(void)
{
    if (!s_active && s_thread < 0) return;
    if (s_active) {
        console_dbgf("[voice] leaving");
        // tell the gateway to leave the voice channel (op4, empty channel)
        gateway_voice_update_ex(s_guild, "", 0, 0);
    }
    s_run = 0;
    if (s_thread >= 0) {
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    s_active = 0; s_connected = 0; s_have_server = 0; s_state_acked = 0;
    s_muted = 0; s_deafened = 0; s_pre_deaf_muted = 0;
    voice_audio_set_muted(0);
    voice_audio_set_deafened(0);
    memset(s_ssrc_map, 0, sizeof(s_ssrc_map));
    s_guild[0] = '\0'; s_chan[0] = '\0';
    s_status = "Idle";
}
