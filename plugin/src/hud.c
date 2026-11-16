#include "../kernel/cordless_mem.h"
#include "../taihen/taihen.h"

#include <psp2/display.h>
#include <psp2/kernel/modulemgr.h>
#include <stdint.h>

static SceUID s_disp_hook = -1;
static tai_hook_ref_t s_disp_ref = 0;

#define HUD_W 104
#define HUD_H 22

typedef struct { char ch; uint8_t rows[7]; } hud_glyph;
static const hud_glyph s_font[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
};

static const uint8_t *find_glyph(char c) {
    for (unsigned i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].ch == c) return s_font[i].rows;
    }
    return s_font[0].rows;
}

static int hook_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *pParam, int sync) {
    int st = cordless_mem_get_overlay();
    if ((st & 1) && pParam && pParam->base &&
        pParam->pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 &&
        pParam->width >= HUD_W + 16 && pParam->height >= HUD_H + 16 &&
        pParam->pitch >= pParam->width) {

        uint32_t *fb = (uint32_t *)pParam->base;
        int pitch = (int)pParam->pitch;
        int px = (int)pParam->width - HUD_W - 10;
        int py = 8;

        int conn  = (st & 2) != 0;
        int muted = (st & 4) != 0;
        int deaf  = (st & 8) != 0;

        for (int y = 0; y < HUD_H; y++) {
            uint32_t *row = fb + (py + y) * pitch + px;
            for (int x = 0; x < HUD_W; x++) {
                row[x] = 0xEE1E1A18;
            }
        }

        uint32_t dot = (deaf || muted) ? 0xFF4444EF :
                       conn ? 0xFF55CC44 : 0xFF33BBEE;
        for (int y = 6; y < 16; y++) {
            uint32_t *row = fb + (py + y) * pitch + px;
            for (int x = 8; x < 18; x++) row[x] = dot;
        }

        const char *lbl = deaf  ? "DEAF" :
                          muted ? "MUTED" :
                          conn  ? "LIVE" : "CONN";
        int tx = px + 26;
        for (; *lbl; lbl++, tx += 7) {
            const uint8_t *g = find_glyph(*lbl);
            for (int ry = 0; ry < 7; ry++) {
                uint8_t bits = g[ry];
                uint32_t *row = fb + (py + 7 + ry) * pitch + tx;
                for (int rx = 0; rx < 5; rx++) {
                    if (bits & (1u << (4 - rx))) row[rx] = 0xFFFFFFFF;
                }
            }
        }
    }
    return TAI_CONTINUE(int, s_disp_ref, pParam, sync);
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;
    s_disp_hook = taiHookFunctionImport(&s_disp_ref, TAI_MAIN_MODULE,
                                        TAI_ANY_LIBRARY, 0x7A410B64,
                                        hook_sceDisplaySetFrameBuf);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;
    if (s_disp_hook >= 0) {
        taiHookRelease(s_disp_hook, s_disp_ref);
        s_disp_hook = -1;
    }
    return SCE_KERNEL_STOP_SUCCESS;
}
