#ifndef CORDLESS_VOICE_IPC_H
#define CORDLESS_VOICE_IPC_H

#define VOICE_IPC_PORT 47321

// Probe whether the background voice plugin is listening on 127.0.0.1:47321.
// Returns 1 if available, 0 if not.
int voice_ipc_available(void);

// Poll status from the background plugin (throttled internally to ~500ms).
void voice_ipc_poll(void);

// IPC-backed equivalents of voice.h operations (used when plugin is active).
int  voice_ipc_join(const char *guild_id, const char *channel_id,
                    const char *user_id, const char *token, const char *cacert_path);
void voice_ipc_leave(void);
void voice_ipc_on_server(const char *token, const char *endpoint,
                         const char *session_id, const char *guild_id,
                         const char *channel_id, const char *user_id);
void voice_ipc_on_state_update(const char *guild_id, const char *channel_id,
                               const char *session_id);
void voice_ipc_set_muted(int muted);
void voice_ipc_set_deafened(int deafened);
void voice_ipc_set_noise_suppress(int enabled);

int         voice_ipc_active(void);
int         voice_ipc_connected(void);
int         voice_ipc_is_muted(void);
int         voice_ipc_is_deafened(void);
const char *voice_ipc_guild_id(void);
const char *voice_ipc_channel_id(void);
const char *voice_ipc_status(void);
int         voice_ipc_is_speaking(const char *user_id);
unsigned    voice_ipc_speaking_mask(void);

#endif // CORDLESS_VOICE_IPC_H
