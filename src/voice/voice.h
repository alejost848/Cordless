#ifndef CORDLESS_VOICE_VOICE_H
#define CORDLESS_VOICE_VOICE_H

#include <stddef.h>

// Discord voice client: join handshake (main-gateway op4 -> VOICE_SERVER_UPDATE
// -> voice-gateway v8), UDP IP discovery, and the Opus + AEAD audio path.

// Build gate for voice join; 0 makes voice_join() refuse.
#ifndef VOICE_JOIN_ENABLE
#define VOICE_JOIN_ENABLE 1
#endif

// Returns 0 if the join request was accepted (the handshake then runs async).
// Pass guild_id == NULL or "" for DM / Group DM calls.
int  voice_join(const char *guild_id, const char *channel_id,
                const char *token, const char *cacert_path);
void voice_leave(void);

// voice_active() is true from join until disconnect; voice_connected() once the
// session description arrives.
int         voice_active(void);
int         voice_connected(void);
const char *voice_guild_id(void);    // guild id of current call ("" for DM/none)
const char *voice_channel_id(void);  // channel we're joining/in ("" if none)
const char *voice_status(void);  // short status text for the UI

int  voice_is_muted(void);
void voice_set_muted(int muted);
void voice_toggle_mute(void);
int  voice_is_deafened(void);
void voice_set_deafened(int deafened);
void voice_toggle_deaf(void);
int      voice_is_speaking(const char *user_id);
unsigned voice_speaking_mask(void);

typedef struct voice_hud_peer {
    char name[20];
    int  speaking;
} voice_hud_peer;

void voice_set_channel_meta(const char *channel_name);
void voice_note_user_name(const char *user_id, const char *name);
int  voice_get_hud_peers(char *out_chan, size_t chan_cap,
                         voice_hud_peer *out_peers, int max_peers);

// Called by the main gateway worker with the voice server assignment
// (VOICE_SERVER_UPDATE token+endpoint, VOICE_STATE_UPDATE session_id).
void voice_on_server(const char *token, const char *endpoint,
                     const char *session_id, const char *guild_id,
                     const char *channel_id, const char *user_id);

// Called by the main gateway worker on VOICE_STATE_UPDATE for the current user.
void voice_on_state_update(const char *guild_id, const char *channel_id,
                           const char *session_id);

#endif  // CORDLESS_VOICE_VOICE_H
