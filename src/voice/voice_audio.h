#ifndef CORDLESS_VOICE_VOICE_AUDIO_H
#define CORDLESS_VOICE_VOICE_AUDIO_H

#include <stddef.h>
#include <stdint.h>

// DAVE (voice E2EE) build gate shared by voice.c and voice_audio.c. 1 (default)
// turns on the inner E2EE layer; -DVOICE_DAVE_ENABLE=OFF advertises no E2EE.
#ifndef VOICE_DAVE_ENABLE
#define VOICE_DAVE_ENABLE 1
#endif

// Voice audio pipeline: RTP + transport AEAD + Opus + mic/speaker, driven by
// voice.c once connected. DAVE inner E2EE wraps the payload when negotiated.

// Offline self-test (KAT vectors + packet round-trips). Returns 0 on pass, else a
// bitmask of failed checks.
int voice_audio_selftest(void);

// Start the RX+TX threads. Borrows voice.c's UDP socket and media params (must
// outlive the pipeline). on_speaking toggles op5 Speaking. Returns 0 on success.
int  voice_audio_start(int udp_fd, unsigned ssrc,
                       const unsigned char secret_key[32], const char *mode,
                       void (*on_speaking)(int on));
void voice_audio_stop(void);
void voice_audio_set_muted(int muted);
int  voice_audio_is_muted(void);
void voice_audio_set_deafened(int deafened);
int  voice_audio_is_deafened(void);
void voice_audio_set_ptt(int enabled, int down);
void voice_audio_set_vad_sens(int sens_level);  // 0=Low, 1=Medium, 2=High
int  voice_audio_is_tx_speaking(void);
int  voice_audio_ssrc_speaking(unsigned ssrc);
unsigned voice_audio_speaking_mask(void);
void voice_audio_play_cue(int kind);  // 0 = connect chime, 1 = mute/deafen blip

#if VOICE_DAVE_ENABLE
// DAVE inner-layer key resolvers registered by voice.c (NULL restores passthrough).
// Each returns 1 with the 16B media key, or 0 to send/treat the frame as plaintext.
typedef struct {
    int (*my_key)(uint8_t generation, uint8_t key[16]);
    int (*sender_key)(unsigned ssrc, uint8_t generation, uint8_t key[16]);
} voice_dave_hooks;
void voice_audio_set_dave(const voice_dave_hooks *h);
// Invalidate cached DAVE media keys on epoch/transition changes (tx_too=1 resets TX key too).
void voice_audio_invalidate_dave_keys(int tx_too);
#endif

#endif  // CORDLESS_VOICE_VOICE_AUDIO_H
