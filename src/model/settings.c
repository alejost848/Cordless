#include "model/settings.h"
#include "ui/theme.h"
#include "voice/voice_audio.h"
#include "console.h"

#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#define DATA_DIR      "ux0:data/cordless"
#define SETTINGS_PATH DATA_DIR "/settings.bin"
#define SETTINGS_MAGIC 0x434C5331u  // 'CLS1'

app_settings g_settings;

static void defaults(void)
{
    memset(&g_settings, 0, sizeof g_settings);
    g_settings.theme       = 0;  // dark
    g_settings.accent      = 0;  // blurple
    g_settings.font_size   = 1;  // medium
    g_settings.density     = 0;  // cozy
    g_settings.clock_24h   = 0;  // 12-hour
    g_settings.open_last      = 0;  // Home
    g_settings.low_battery    = 1;  // idle skip on
    g_settings.noise_suppress = 0;  // off by default (experimental)
}

void settings_load(void)
{
    defaults();
    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        unsigned magic = 0;
        app_settings s = g_settings;
        int m = sceIoRead(fd, &magic, sizeof magic);
        int r = sceIoRead(fd, &s, sizeof s);
        sceIoClose(fd);
        if (m == (int)sizeof magic && magic == SETTINGS_MAGIC && r > 0)
            g_settings = s;
    }
    g_settings.noise_suppress = g_settings.noise_suppress ? 1 : 0;
    theme_apply_all();
    voice_audio_set_noise_suppress(g_settings.noise_suppress);
}

void settings_save(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) { console_logf("[settings] save open failed 0x%08x", (unsigned)fd); return; }
    unsigned magic = SETTINGS_MAGIC;
    sceIoWrite(fd, &magic, sizeof magic);
    sceIoWrite(fd, &g_settings, sizeof g_settings);
    sceIoSyncByFd(fd, 0);
    sceIoClose(fd);
    sceIoSync("ux0:", 0);
}
