#include "../kernel/cordless_mem.h"
#include "../taihen/taihen.h"

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/modulemgr.h>
#include <stddef.h>
#include <stdint.h>

/* Provide freestanding memset/memcpy/strlen so -nostdlib links cleanly even when GCC
 * emits block or string builtins under -O2. */
#pragma GCC push_options
#pragma GCC optimize("no-tree-loop-distribute-patterns")
void *memset(void *dst, int c, size_t n) {
    volatile uint8_t *p = (volatile uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    for (size_t i = 0; i < n; i++) p[i] = v;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t n) {
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const volatile uint8_t *s = (const volatile uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
size_t strlen(const char *s) {
    const volatile char *p = (const volatile char *)s;
    size_t n = 0;
    while (p[n]) n++;
    return n;
}
#pragma GCC pop_options

#define NUM_CTRL_HOOKS 12

static SceUID         s_disp_hook = -1;
static tai_hook_ref_t s_disp_ref = 0;
static SceUID         s_ctrl_hooks[NUM_CTRL_HOOKS];
static tai_hook_ref_t s_ctrl_refs[NUM_CTRL_HOOKS];

static volatile int      s_panel_open = 0;
static volatile int      s_sel = 0;
static volatile uint64_t s_last_ts[2];
static volatile uint32_t s_prev_btn[2];
static volatile int      s_combo_prev[2];
static volatile uint32_t s_swallow_mask[2];

#define AUDIO_MAX_PORTS 8
#define NUM_AUDIO_HOOKS 4

static int            s_in_shell = 0;
static SceUID         s_audio_hooks[NUM_AUDIO_HOOKS] = {-1, -1, -1, -1};
static tai_hook_ref_t s_audio_refs[NUM_AUDIO_HOOKS];

/* Track per-port unscaled base volume (L/R); slot state: 0=closed, 1=hooked, 2=probed. */
static uint8_t s_port_active[AUDIO_MAX_PORTS];
static int     s_port_base_l[AUDIO_MAX_PORTS];
static int     s_port_base_r[AUDIO_MAX_PORTS];

static volatile int s_eff_vol_pct = 100;
static int          s_probe_tick  = 0;

static int scale_vol(int base, int pct) {
    if (base < 0) base = 0;
    if (base > SCE_AUDIO_OUT_MAX_VOL) base = SCE_AUDIO_OUT_MAX_VOL;
    if (pct <= 0) return 0;
    if (pct >= 100) return base;
    /* Quadratic taper gives smooth perceived volume steps across 0..100%. */
    return (int)(((uint32_t)base * (uint32_t)pct * (uint32_t)pct) / 10000u);
}

static void apply_port_vol(int port, int pct) {
    int sv[2];
    sv[0] = scale_vol(s_port_base_l[port], pct);
    sv[1] = scale_vol(s_port_base_r[port], pct);
    /* Direct weak-stub call in cordless_hud bypasses TAI_MAIN_MODULE import hooks. */
    sceAudioOutSetVolume(port,
                         (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                         sv);
}

static void probe_audio_ports(int eff_pct) {
    for (int p = 0; p < AUDIO_MAX_PORTS; p++) {
        if (s_port_active[p] == 0) {
            if (sceAudioOutGetConfig(p, SCE_AUDIO_OUT_CONFIG_TYPE_LEN) >= 0) {
                s_port_base_l[p] = SCE_AUDIO_OUT_MAX_VOL;
                s_port_base_r[p] = SCE_AUDIO_OUT_MAX_VOL;
                s_port_active[p] = 2;
                apply_port_vol(p, eff_pct);
            }
        } else if (s_port_active[p] == 2) {
            if (sceAudioOutGetConfig(p, SCE_AUDIO_OUT_CONFIG_TYPE_LEN) < 0) {
                s_port_active[p] = 0;
            }
        }
    }
}

static void sync_audio_volumes(int eff_pct, int allow_probe) {
    if (s_in_shell) return;
    if (eff_pct < 0) eff_pct = 0;
    if (eff_pct > 100) eff_pct = 100;

    if (eff_pct != s_eff_vol_pct) {
        s_eff_vol_pct = eff_pct;
        s_probe_tick = 0;
        for (int p = 0; p < AUDIO_MAX_PORTS; p++) {
            if (s_port_active[p]) {
                apply_port_vol(p, eff_pct);
            }
        }
        if (eff_pct < 100 || s_audio_hooks[2] < 0) {
            probe_audio_ports(eff_pct);
        }
        return;
    }

    if (allow_probe && eff_pct < 100) {
        if (++s_probe_tick >= 60) {
            s_probe_tick = 0;
            probe_audio_ports(eff_pct);
        }
    }
}

static int hook_sceAudioOutOpenPort(SceAudioOutPortType type, int len,
                                    int freq, SceAudioOutMode mode) {
    int port = TAI_CONTINUE(int, s_audio_refs[0], type, len, freq, mode);
    if (port >= 0 && port < AUDIO_MAX_PORTS) {
        s_port_base_l[port] = SCE_AUDIO_OUT_MAX_VOL;
        s_port_base_r[port] = SCE_AUDIO_OUT_MAX_VOL;
        s_port_active[port] = 1;
        if (s_eff_vol_pct < 100) {
            apply_port_vol(port, s_eff_vol_pct);
        }
    }
    return port;
}

static int hook_sceAudioOutSetVolume(int port, SceAudioOutChannelFlag ch, int *vol) {
    if (!vol || port < 0 || port >= AUDIO_MAX_PORTS) {
        return TAI_CONTINUE(int, s_audio_refs[1], port, ch, vol);
    }
    if (!s_port_active[port]) {
        s_port_base_l[port] = SCE_AUDIO_OUT_MAX_VOL;
        s_port_base_r[port] = SCE_AUDIO_OUT_MAX_VOL;
        s_port_active[port] = 1;
    }
    int sv[2];
    int idx = 0;
    if (ch & SCE_AUDIO_VOLUME_FLAG_L_CH) {
        int v = vol[idx];
        if (v < 0) v = 0;
        if (v > SCE_AUDIO_OUT_MAX_VOL) v = SCE_AUDIO_OUT_MAX_VOL;
        s_port_base_l[port] = v;
        sv[idx] = scale_vol(v, s_eff_vol_pct);
        idx++;
    }
    if (ch & SCE_AUDIO_VOLUME_FLAG_R_CH) {
        int v = vol[idx];
        if (v < 0) v = 0;
        if (v > SCE_AUDIO_OUT_MAX_VOL) v = SCE_AUDIO_OUT_MAX_VOL;
        s_port_base_r[port] = v;
        sv[idx] = scale_vol(v, s_eff_vol_pct);
        idx++;
    }
    return TAI_CONTINUE(int, s_audio_refs[1], port, ch, idx ? sv : vol);
}

static int hook_sceAudioOutOutput(int port, const void *buf) {
    if (port >= 0 && port < AUDIO_MAX_PORTS && !s_port_active[port]) {
        s_port_base_l[port] = SCE_AUDIO_OUT_MAX_VOL;
        s_port_base_r[port] = SCE_AUDIO_OUT_MAX_VOL;
        s_port_active[port] = 1;
        if (s_eff_vol_pct < 100) {
            apply_port_vol(port, s_eff_vol_pct);
        }
    }
    return TAI_CONTINUE(int, s_audio_refs[2], port, buf);
}

static int hook_sceAudioOutReleasePort(int port) {
    if (port >= 0 && port < AUDIO_MAX_PORTS) {
        s_port_active[port] = 0;
    }
    return TAI_CONTINUE(int, s_audio_refs[3], port);
}

static void fmt_pct_tag(char out[7], int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    out[0] = '<';
    out[1] = (pct >= 100) ? ('0' + (pct / 100)) : ' ';
    out[2] = (pct >= 10)  ? ('0' + ((pct / 10) % 10)) : ' ';
    out[3] = (char)('0' + (pct % 10));
    out[4] = '%';
    out[5] = '>';
    out[6] = '\0';
}

/* 5x7 font bitmaps for printable ASCII 0x20..0x7E (5 bits wide, MSB=leftmost). */
static const uint8_t s_font95[95][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* '!' */
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}, /* '#' */
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* '$' */
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* '%' */
    {0x08,0x14,0x14,0x08,0x15,0x12,0x0D}, /* '&' */
    {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, /* '\'' */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* '(' */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ')' */
    {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00}, /* '*' */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x04,0x04,0x08}, /* ',' */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x04,0x04}, /* '.' */
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, /* '/' */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* '0' */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* '1' */
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, /* '2' */
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, /* '3' */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* '4' */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* '5' */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* '6' */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* '7' */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* '8' */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* '9' */
    {0x00,0x04,0x04,0x00,0x04,0x04,0x00}, /* ':' */
    {0x00,0x04,0x04,0x00,0x04,0x04,0x08}, /* ';' */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* '<' */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* '=' */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* '>' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* '?' */
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, /* '@' */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* 'A' */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* 'B' */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* 'C' */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* 'D' */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* 'E' */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* 'F' */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* 'G' */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* 'H' */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* 'I' */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* 'J' */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* 'K' */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* 'L' */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* 'M' */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* 'N' */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 'O' */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* 'P' */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* 'Q' */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* 'R' */
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, /* 'S' */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* 'T' */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 'U' */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* 'V' */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* 'W' */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* 'X' */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* 'Y' */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* 'Z' */
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* '[' */
    {0x10,0x10,0x08,0x04,0x02,0x01,0x01}, /* '\\' */
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ']' */
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* '_' */
    {0x08,0x04,0x02,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, /* 'a' */
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, /* 'b' */
    {0x00,0x00,0x0F,0x10,0x10,0x10,0x0F}, /* 'c' */
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, /* 'd' */
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, /* 'e' */
    {0x06,0x08,0x1E,0x08,0x08,0x08,0x08}, /* 'f' */
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}, /* 'g' */
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, /* 'h' */
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, /* 'i' */
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, /* 'j' */
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, /* 'k' */
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, /* 'l' */
    {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, /* 'm' */
    {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, /* 'n' */
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, /* 'o' */
    {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, /* 'p' */
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01}, /* 'q' */
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, /* 'r' */
    {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}, /* 's' */
    {0x08,0x08,0x1E,0x08,0x08,0x09,0x06}, /* 't' */
    {0x00,0x00,0x11,0x11,0x11,0x11,0x0F}, /* 'u' */
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, /* 'v' */
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, /* 'w' */
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, /* 'x' */
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, /* 'y' */
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, /* 'z' */
    {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, /* '{' */
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, /* '|' */
    {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, /* '}' */
    {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, /* '~' */
};

static int hud_strlen(const char *s) {
    return s ? (int)strlen(s) : 0;
}

static void draw_rect(uint32_t *fb, int pitch, int fw, int fh,
                      int x, int y, int w, int h, uint32_t col) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > fw) ? fw : (x + w);
    int y1 = (y + h > fh) ? fh : (y + h);
    for (int py = y0; py < y1; py++) {
        uint32_t *row = fb + py * pitch;
        for (int px = x0; px < x1; px++) {
            row[px] = col;
        }
    }
}

static void draw_border(uint32_t *fb, int pitch, int fw, int fh,
                        int x, int y, int w, int h, uint32_t col) {
    draw_rect(fb, pitch, fw, fh, x, y, w, 1, col);
    draw_rect(fb, pitch, fw, fh, x, y + h - 1, w, 1, col);
    draw_rect(fb, pitch, fw, fh, x, y, 1, h, col);
    draw_rect(fb, pitch, fw, fh, x + w - 1, y, 1, h, col);
}

static void draw_text(uint32_t *fb, int pitch, int fw, int fh,
                      int x, int y, uint32_t col, const char *s, int max_chars) {
    if (!s) return;
    for (int idx = 0; *s && idx < max_chars; s++, idx++, x += 6) {
        unsigned char uc = (unsigned char)*s;
        if (uc < 0x20 || uc > 0x7E) uc = '?';
        const uint8_t *g = s_font95[uc - 0x20];
        for (int ry = 0; ry < 7; ry++) {
            int py = y + ry;
            if (py < 0 || py >= fh) continue;
            uint8_t bits = g[ry];
            uint32_t *row = fb + py * pitch;
            for (int rx = 0; rx < 5; rx++) {
                int px = x + rx;
                if (px >= 0 && px < fw && (bits & (1u << (4 - rx)))) {
                    row[px] = col;
                }
            }
        }
    }
}

/* ABGR colors (SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) */
#define COL_BG        0xFF221F1E /* #1E1F22 */
#define COL_HDR       0xFF312D2B /* #2B2D31 */
#define COL_BORDER    0xFF47413F /* #3F4147 */
#define COL_DIV       0xFF3A3634 /* #34363A */
#define COL_BTN       0xFF34302E /* #2E3034 */
#define COL_BLURPLE   0xFFF26558 /* #5865F2 */
#define COL_DANGER    0xFF3C37DA /* #DA373C */
#define COL_WHITE     0xFFFFFFFF
#define COL_DIM       0xFFA49B94 /* #949BA4 */
#define COL_GREEN     0xFF5AA523 /* #23A55A */
#define COL_TALK      0xFF6CD943 /* #43D96C */
#define COL_RED       0xFF433FF2 /* #F23F43 */
#define COL_AMBER     0xFF32B2F0 /* #F0B232 */
#define COL_PEER_IDLE 0xFF5E5753

static void render_pill(uint32_t *fb, int pitch, int fw, int fh, int flags) {
    const int pw = 104, ph = 22;
    if (fw < pw + 16 || fh < ph + 16) return;
    int px = fw - pw - 10;
    int py = 8;

    int conn  = (flags & CORDLESS_OV_CONNECTED) != 0;
    int muted = (flags & CORDLESS_OV_MUTED) != 0;
    int deaf  = (flags & CORDLESS_OV_DEAF) != 0;

    draw_rect(fb, pitch, fw, fh, px, py, pw, ph, COL_BG);
    draw_border(fb, pitch, fw, fh, px, py, pw, ph, COL_BORDER);

    uint32_t dot = (deaf || muted) ? COL_RED : (conn ? COL_GREEN : COL_AMBER);
    draw_rect(fb, pitch, fw, fh, px + 8, py + 6, 10, 10, dot);

    const char *lbl = deaf ? "DEAF" : (muted ? "MUTED" : (conn ? "LIVE" : "CONN"));
    draw_text(fb, pitch, fw, fh, px + 26, py + 7, COL_WHITE, lbl, 8);
}

static void render_panel(uint32_t *fb, int pitch, int fw, int fh,
                         const cordless_overlay_state *st) {
    const int pw = 252;
    int peers = (int)st->peer_count;
    if (peers > CORDLESS_HUD_MAX_PEERS) peers = CORDLESS_HUD_MAX_PEERS;
    int peer_rows = 1 + (peers > 0 ? peers : 1);
    int ph = 26 + 16 + peer_rows * 15 + 8 + 4 * 24 + 18;
    if (fw < pw + 16 || fh < ph + 16) {
        render_pill(fb, pitch, fw, fh, (int)st->flags);
        return;
    }

    int px = fw - pw - 10;
    int py = 8;
    int conn  = (st->flags & CORDLESS_OV_CONNECTED) != 0;
    int muted = (st->flags & CORDLESS_OV_MUTED) != 0;
    int deaf  = (st->flags & CORDLESS_OV_DEAF) != 0;
    int tx    = (st->flags & CORDLESS_OV_TX_SPEAKING) != 0;

    /* Card backdrop + border */
    draw_rect(fb, pitch, fw, fh, px, py, pw, ph, COL_BG);
    draw_border(fb, pitch, fw, fh, px, py, pw, ph, COL_BORDER);

    /* Header */
    draw_rect(fb, pitch, fw, fh, px + 1, py + 1, pw - 2, 24, COL_HDR);
    draw_rect(fb, pitch, fw, fh, px + 1, py + 25, pw - 2, 1, COL_DIV);
    uint32_t hdr_dot = (deaf || muted) ? COL_RED : (conn ? COL_GREEN : COL_AMBER);
    draw_rect(fb, pitch, fw, fh, px + 10, py + 9, 8, 8, hdr_dot);

    const char *state_tag = deaf ? "DEAF" : (muted ? "MUTED" : (conn ? "LIVE" : "CONN"));
    draw_text(fb, pitch, fw, fh, px + 24, py + 9, COL_WHITE, state_tag, 6);
    int tag_len = hud_strlen(state_tag);
    const char *ch = st->channel[0] ? st->channel : "Voice Call";
    draw_text(fb, pitch, fw, fh, px + 24 + tag_len * 6 + 6, py + 9, COL_DIM, ch,
              32 - tag_len);

    /* Participants section */
    int cy = py + 31;
    draw_text(fb, pitch, fw, fh, px + 10, cy, COL_DIM,
              conn ? "SPEAKERS" : "CONNECTING...", 20);
    cy += 13;

    /* Self row */
    uint32_t self_dot = (deaf || muted) ? COL_RED : (tx ? COL_TALK : COL_PEER_IDLE);
    draw_rect(fb, pitch, fw, fh, px + 12, cy + 2, 6, 6, self_dot);
    draw_text(fb, pitch, fw, fh, px + 24, cy + 1, COL_WHITE, "You", 16);
    const char *self_lbl = deaf ? "DEAF" : (muted ? "MUTED" : (tx ? "TALK" : "MIC ON"));
    uint32_t self_col = (deaf || muted) ? COL_RED : (tx ? COL_TALK : COL_DIM);
    int slen = hud_strlen(self_lbl);
    draw_text(fb, pitch, fw, fh, px + pw - 10 - slen * 6, cy + 1, self_col, self_lbl, 8);
    cy += 15;

    if (peers == 0) {
        draw_text(fb, pitch, fw, fh, px + 24, cy + 1, COL_DIM,
                  conn ? "(No other active speakers)" : "(Joining channel...)", 34);
        cy += 15;
    } else {
        for (int i = 0; i < peers; i++) {
            int pspk = (st->peers[i].flags & CORDLESS_PEER_SPEAKING) != 0;
            draw_rect(fb, pitch, fw, fh, px + 12, cy + 2, 6, 6, pspk ? COL_TALK : COL_PEER_IDLE);
            draw_text(fb, pitch, fw, fh, px + 24, cy + 1,
                      pspk ? COL_WHITE : COL_DIM, st->peers[i].name, 26);
            if (pspk) {
                draw_text(fb, pitch, fw, fh, px + pw - 10 - 4 * 6, cy + 1, COL_TALK, "TALK", 4);
            }
            cy += 15;
        }
    }

    /* Divider + Actions */
    cy += 3;
    draw_rect(fb, pitch, fw, fh, px + 8, cy, pw - 16, 1, COL_DIV);
    cy += 5;

    const char *btns[4];
    btns[0] = muted ? "Unmute Microphone" : "Mute Microphone";
    btns[1] = deaf  ? "Undeafen Audio"    : "Deafen Audio";
    btns[2] = "Game Vol";
    btns[3] = "Disconnect";

    int gvol = (int)st->game_vol_pct;
    if (gvol < 0) gvol = 0;
    if (gvol > 100) gvol = 100;

    int sel = s_sel;
    for (int i = 0; i < 4; i++) {
        uint32_t bcol = COL_BTN;
        if (i == sel) bcol = (i == 3) ? COL_DANGER : COL_BLURPLE;
        draw_rect(fb, pitch, fw, fh, px + 10, cy, pw - 20, 20, bcol);
        if (i == sel) {
            draw_text(fb, pitch, fw, fh, px + 16, cy + 6, COL_WHITE, ">", 1);
        }
        draw_text(fb, pitch, fw, fh, px + 26, cy + 6,
                  (i == sel) ? COL_WHITE : 0xFFE1DEDB, btns[i], 30);
        if (i == 2) {
            const int bx = px + 82;
            const int bw = 104;
            int fill = (gvol * bw) / 100;
            draw_rect(fb, pitch, fw, fh, bx, cy + 6, bw, 8, 0xFF1A1716);
            if (fill > 0) {
                draw_rect(fb, pitch, fw, fh, bx, cy + 6, fill, 8,
                          (i == sel) ? COL_WHITE : COL_BLURPLE);
            }
            char ptag[7];
            fmt_pct_tag(ptag, gvol);
            draw_text(fb, pitch, fw, fh, px + 194, cy + 6,
                      (i == sel) ? COL_WHITE : 0xFFE1DEDB, ptag, 6);
        }
        cy += 24;
    }

    draw_text(fb, pitch, fw, fh, px + 10, cy + 2, COL_DIM,
              (sel == 2) ? "L/R:Vol  X:40/100%  O:Close" : "D-Pad:Nav  X:Select  O:Close",
              36);
}

static int hook_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *pParam, int sync) {
    int quick = cordless_mem_get_overlay();
    if (!s_in_shell) {
        int eff_pct = (quick & CORDLESS_OV_ACTIVE) ? cordless_mem_get_game_vol() : 100;
        sync_audio_volumes(eff_pct, 1);
    }
    if (!(quick & CORDLESS_OV_ACTIVE)) {
        if (s_panel_open) s_panel_open = 0;
    } else if (pParam && pParam->base &&
               pParam->pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 &&
               pParam->pitch >= pParam->width) {
        uint32_t *fb = (uint32_t *)pParam->base;
        int pitch = (int)pParam->pitch;
        int fw = (int)pParam->width;
        int fh = (int)pParam->height;

        if (s_panel_open) {
            cordless_overlay_state st;
            memset(&st, 0, sizeof(st));
            if (cordless_mem_get_overlay_state(&st) == 0 && (st.flags & CORDLESS_OV_ACTIVE)) {
                render_panel(fb, pitch, fw, fh, &st);
            } else {
                s_panel_open = 0;
            }
        } else {
            render_pill(fb, pitch, fw, fh, quick);
        }
    }
    return TAI_CONTINUE(int, s_disp_ref, pParam, sync);
}

static void mask_pad_sample(SceCtrlData *d, uint32_t mask, int is_neg) {
    uint32_t m = mask & 0x0000FFFFu;
    if (!m) return;
    if (is_neg) {
        d->buttons |= m;
    } else {
        d->buttons &= ~m;
    }
    if (m == 0x0000FFFFu) {
        d->lx = 128;
        d->ly = 128;
        d->rx = 128;
        d->ry = 128;
    }
    if (m & SCE_CTRL_UP)       d->up = 0;
    if (m & SCE_CTRL_RIGHT)    d->right = 0;
    if (m & SCE_CTRL_DOWN)     d->down = 0;
    if (m & SCE_CTRL_LEFT)     d->left = 0;
    if (m & SCE_CTRL_LTRIGGER) d->lt = 0;
    if (m & SCE_CTRL_RTRIGGER) d->rt = 0;
    if (m & SCE_CTRL_L1)       d->l1 = 0;
    if (m & SCE_CTRL_R1)       d->r1 = 0;
    if (m & SCE_CTRL_TRIANGLE) d->triangle = 0;
    if (m & SCE_CTRL_CIRCLE)   d->circle = 0;
    if (m & SCE_CTRL_CROSS)    d->cross = 0;
    if (m & SCE_CTRL_SQUARE)   d->square = 0;
}

static int ctrl_process(int port, SceCtrlData *pad_data, int count, int ret, int is_neg) {
    if (ret <= 0 || !pad_data) return ret;

    int n = (ret < count) ? ret : count;
    int closing = 0;
    if (port == 0 || port == 1) {
        const SceCtrlData *cur = (n > 1 && pad_data[n - 1].timeStamp > pad_data[0].timeStamp)
                                 ? &pad_data[n - 1] : &pad_data[0];
        uint32_t raw = is_neg ? ~cur->buttons : cur->buttons;
        uint64_t ts = cur->timeStamp;
        int combo_now = ((raw & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) != 0) &&
                        ((raw & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) != 0) &&
                        ((raw & SCE_CTRL_TRIANGLE) != 0);

        if (ts != s_last_ts[port] || (ts == 0 && raw != s_prev_btn[port])) {
            s_last_ts[port] = ts;
            s_swallow_mask[port] &= (raw & 0x0000FFFFu);
            uint32_t pressed = raw & ~s_prev_btn[port];
            s_prev_btn[port] = raw;

            if (combo_now && !s_combo_prev[port]) {
                if (s_panel_open) {
                    s_swallow_mask[port] |= (raw & 0x0000FFFFu);
                    s_panel_open = 0;
                    closing = 1;
                } else if (cordless_mem_get_overlay() & CORDLESS_OV_ACTIVE) {
                    s_panel_open = 1;
                    s_sel = 0;
                    s_swallow_mask[port] = 0;
                }
            } else if (s_panel_open && !combo_now) {
                if (pressed & SCE_CTRL_UP) {
                    s_sel = (s_sel > 0) ? (s_sel - 1) : 3;
                } else if (pressed & SCE_CTRL_DOWN) {
                    s_sel = (s_sel + 1) % 4;
                } else if (pressed & SCE_CTRL_CIRCLE) {
                    s_swallow_mask[port] |= (raw & 0x0000FFFFu);
                    s_panel_open = 0;
                    closing = 1;
                } else if (s_sel == 2 && (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS))) {
                    int v = cordless_mem_get_game_vol();
                    if (pressed & SCE_CTRL_LEFT) {
                        v -= 10;
                    } else if (pressed & SCE_CTRL_RIGHT) {
                        v += 10;
                    } else {
                        v = (v < 100) ? 100 : 40;
                    }
                    if (v < 0) v = 0;
                    if (v > 100) v = 100;
                    cordless_mem_set_game_vol(v);
                    sync_audio_volumes(v, 0);
                } else if (pressed & SCE_CTRL_CROSS) {
                    if (s_sel == 0) {
                        cordless_mem_post_action(CORDLESS_ACT_MUTE);
                    } else if (s_sel == 1) {
                        cordless_mem_post_action(CORDLESS_ACT_DEAF);
                    } else if (s_sel == 3) {
                        cordless_mem_post_action(CORDLESS_ACT_LEAVE);
                        s_swallow_mask[port] |= (raw & 0x0000FFFFu);
                        s_panel_open = 0;
                        closing = 1;
                    }
                }
            }
            s_combo_prev[port] = combo_now;
        }
    }

    uint32_t mask = (s_panel_open || closing)
                    ? 0x0000FFFFu
                    : ((port == 0 || port == 1) ? s_swallow_mask[port] : 0u);
    if (mask) {
        for (int i = 0; i < n; i++) {
            mask_pad_sample(&pad_data[i], mask, is_neg);
        }
    }
    return ret;
}

#define DEFINE_CTRL_HOOK(idx, is_neg) \
static int hook_ctrl_##idx(int port, SceCtrlData *pad_data, int count) { \
    int ret = TAI_CONTINUE(int, s_ctrl_refs[idx], port, pad_data, count); \
    return ctrl_process(port, pad_data, count, ret, (is_neg)); \
}

DEFINE_CTRL_HOOK(0, 0)
DEFINE_CTRL_HOOK(1, 0)
DEFINE_CTRL_HOOK(2, 0)
DEFINE_CTRL_HOOK(3, 0)
DEFINE_CTRL_HOOK(4, 0)
DEFINE_CTRL_HOOK(5, 0)
DEFINE_CTRL_HOOK(6, 0)
DEFINE_CTRL_HOOK(7, 0)
DEFINE_CTRL_HOOK(8, 1)
DEFINE_CTRL_HOOK(9, 1)
DEFINE_CTRL_HOOK(10, 1)
DEFINE_CTRL_HOOK(11, 1)

typedef int (*ctrl_hook_fn)(int port, SceCtrlData *pad_data, int count);

static const struct {
    uint32_t     nid;
    ctrl_hook_fn fn;
} s_ctrl_targets[NUM_CTRL_HOOKS] = {
    { 0xA9C3CED6, hook_ctrl_0  }, /* sceCtrlPeekBufferPositive */
    { 0x15F81E8C, hook_ctrl_1  }, /* sceCtrlPeekBufferPositive2 */
    { 0xA59454D3, hook_ctrl_2  }, /* sceCtrlPeekBufferPositiveExt */
    { 0x860BF292, hook_ctrl_3  }, /* sceCtrlPeekBufferPositiveExt2 */
    { 0x67E7AB83, hook_ctrl_4  }, /* sceCtrlReadBufferPositive */
    { 0xC4226A3E, hook_ctrl_5  }, /* sceCtrlReadBufferPositive2 */
    { 0xE2D99296, hook_ctrl_6  }, /* sceCtrlReadBufferPositiveExt */
    { 0xA7178860, hook_ctrl_7  }, /* sceCtrlReadBufferPositiveExt2 */
    { 0x104ED1A7, hook_ctrl_8  }, /* sceCtrlPeekBufferNegative */
    { 0x81A89660, hook_ctrl_9  }, /* sceCtrlPeekBufferNegative2 */
    { 0x15F96FB0, hook_ctrl_10 }, /* sceCtrlReadBufferNegative */
    { 0x27A0C5FB, hook_ctrl_11 }, /* sceCtrlReadBufferNegative2 */
};

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;
    tai_module_info_t minfo;
    memset(&minfo, 0, sizeof(minfo));
    minfo.size = sizeof(minfo);
    s_in_shell = (taiGetModuleInfo("SceShell", &minfo) >= 0) ? 1 : 0;

    s_disp_hook = taiHookFunctionImport(&s_disp_ref, TAI_MAIN_MODULE,
                                        TAI_ANY_LIBRARY, 0x7A410B64,
                                        hook_sceDisplaySetFrameBuf);
    for (int i = 0; i < NUM_CTRL_HOOKS; i++) {
        s_ctrl_hooks[i] = taiHookFunctionImport(&s_ctrl_refs[i], TAI_MAIN_MODULE,
                                                TAI_ANY_LIBRARY, s_ctrl_targets[i].nid,
                                                s_ctrl_targets[i].fn);
    }
    if (!s_in_shell) {
        s_audio_hooks[0] = taiHookFunctionImport(&s_audio_refs[0], TAI_MAIN_MODULE,
                                                 TAI_ANY_LIBRARY, 0x5BC341E4u,
                                                 hook_sceAudioOutOpenPort);
        s_audio_hooks[1] = taiHookFunctionImport(&s_audio_refs[1], TAI_MAIN_MODULE,
                                                 TAI_ANY_LIBRARY, 0x64167F11u,
                                                 hook_sceAudioOutSetVolume);
        s_audio_hooks[2] = taiHookFunctionImport(&s_audio_refs[2], TAI_MAIN_MODULE,
                                                 TAI_ANY_LIBRARY, 0x02DB3F5Fu,
                                                 hook_sceAudioOutOutput);
        s_audio_hooks[3] = taiHookFunctionImport(&s_audio_refs[3], TAI_MAIN_MODULE,
                                                 TAI_ANY_LIBRARY, 0x69E2E6B5u,
                                                 hook_sceAudioOutReleasePort);
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;
    s_panel_open = 0;
    if (!s_in_shell) {
        sync_audio_volumes(100, 0);
        for (int i = 0; i < NUM_AUDIO_HOOKS; i++) {
            if (s_audio_hooks[i] >= 0) {
                taiHookRelease(s_audio_hooks[i], s_audio_refs[i]);
                s_audio_hooks[i] = -1;
            }
        }
    }
    for (int i = 0; i < NUM_CTRL_HOOKS; i++) {
        if (s_ctrl_hooks[i] >= 0) {
            taiHookRelease(s_ctrl_hooks[i], s_ctrl_refs[i]);
            s_ctrl_hooks[i] = -1;
        }
    }
    if (s_disp_hook >= 0) {
        taiHookRelease(s_disp_hook, s_disp_ref);
        s_disp_hook = -1;
    }
    return SCE_KERNEL_STOP_SUCCESS;
}
