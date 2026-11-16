#ifndef CORDLESS_MEDIA_AUDIO_PLAYER_H
#define CORDLESS_MEDIA_AUDIO_PLAYER_H

// Ogg Opus voice-message / audio attachment player (Vita sceAudioOut + libopus).
// One clip plays at a time on a background thread; calling toggle on the active
// message stops playback, and calling it on another switches immediately.

void audio_player_toggle(const char *msg_id, const char *url, const char *cacert_path);
void audio_player_stop(void);

// Returns 0 = idle (or playing a different message), 1 = loading, 2 = playing.
// When non-zero, fills *pos_ms and *dur_ms (if non-NULL).
int  audio_player_status(const char *msg_id, int *pos_ms, int *dur_ms);

// Voice message recorder (16kHz mono mic -> Ogg/Opus -> Discord upload).
int  audio_rec_start(void);
void audio_rec_stop(int send, const char *channel_id, const char *token, const char *cacert_path);
// Returns 0 = idle, 1 = recording, 2 = uploading. Fills *ms_out and *level_out (0..255).
int  audio_rec_status(int *ms_out, int *level_out);

#endif  // CORDLESS_MEDIA_AUDIO_PLAYER_H
