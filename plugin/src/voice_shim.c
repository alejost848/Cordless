#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <stdarg.h>
#include <stddef.h>

#define LOG_PATH "ur0:data/cordless_voice_poc.log"

void console_logf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = sceClibVsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (len <= 0) return;
    if (len >= (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 2;
    buf[len++] = '\n';

    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
}

void console_dbgf(const char *fmt, ...) {
    (void)fmt;
}

/* Minimal stubs for voice.c hooks when running standalone in the plugin */
void store_lock(void) {}
void store_unlock(void) {}
void store_touch(void) {}
const char *store_me_id(void) { return ""; }
void store_voice_set(const char *guild_id, const char *channel_id,
                     const char *user_id, const char *session_id,
                     const int *mute, const int *deaf) {
    (void)guild_id; (void)channel_id; (void)user_id;
    (void)session_id; (void)mute; (void)deaf;
}
int discord_ring_call(const char *channel_id, const char *token, const char *cacert_path) {
    (void)channel_id; (void)token; (void)cacert_path;
    return 0;
}
