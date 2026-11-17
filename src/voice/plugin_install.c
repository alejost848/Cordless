#include "voice/plugin_install.h"
#include "voice/voice_ipc.h"
#include "console.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAI_DIR        "ur0:tai"
#define TAI_CONFIG     "ur0:tai/config.txt"
#define TAI_CONFIG_BAK "ur0:tai/config.txt.cordless.bak"
#define TAI_CONFIG_TMP "ur0:tai/config.txt.tmp"

static const char *const k_plugin_files[4] = {
    "cordless_mem.skprx",
    "cordless_voice_poc.suprx",
    "cordless_voice_core.suprx",
    "cordless_hud.suprx",
};

static int file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static int find_plugin_src(const char *name, char *out, size_t outsz) {
    snprintf(out, outsz, "app0:data/plugins/%s", name);
    if (file_exists(out)) return 1;
    snprintf(out, outsz, "ux0:data/%s", name);
    if (file_exists(out)) return 1;
    return 0;
}

static int write_all(SceUID fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int w = sceIoWrite(fd, buf + off, len - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    SceUID rfd = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (rfd < 0) return -1;
    SceUID wfd = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (wfd < 0) { sceIoClose(rfd); return -1; }

    char *buf = malloc(16384);
    if (!buf) { sceIoClose(rfd); sceIoClose(wfd); return -1; }

    int rc = 0;
    for (;;) {
        int n = sceIoRead(rfd, buf, 16384);
        if (n < 0) { rc = -1; break; }
        if (n == 0) break;
        if (write_all(wfd, buf, (size_t)n) < 0) { rc = -1; break; }
    }
    free(buf);
    sceIoClose(rfd);
    sceIoClose(wfd);
    return rc;
}

static char *read_text_file(const char *path, size_t *out_len) {
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) return NULL;
    if (st.st_size < 0 || st.st_size > 256 * 1024) return NULL;
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf) return NULL;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) { free(buf); return NULL; }
    size_t off = 0;
    while (off < sz) {
        int n = sceIoRead(fd, buf + off, sz - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    sceIoClose(fd);
    buf[off] = '\0';
    if (out_len) *out_len = off;
    return buf;
}

static int is_cordless_line(const char *line, size_t len) {
    for (int i = 0; i < 4; i++) {
        const char *needle = k_plugin_files[i];
        size_t nlen = strlen(needle);
        if (len >= nlen) {
            for (size_t j = 0; j + nlen <= len; j++) {
                if (memcmp(line + j, needle, nlen) == 0) return 1;
            }
        }
    }
    return 0;
}

static int line_equals_trimmed(const char *line, size_t len, const char *target) {
    while (len > 0 && (*line == ' ' || *line == '\t')) { line++; len--; }
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r')) len--;
    return strlen(target) == len && memcmp(line, target, len) == 0;
}

static int config_has_all_entries(void) {
    size_t sz = 0;
    char *txt = read_text_file(TAI_CONFIG, &sz);
    if (!txt) return 0;

    int has_mem = 0, has_poc = 0, has_hud = 0;
    const char *p = txt, *end = txt + sz;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (len > 0 && p[0] != '#') {
            if (line_equals_trimmed(p, len, "ur0:tai/cordless_mem.skprx")) has_mem = 1;
            else if (line_equals_trimmed(p, len, "ur0:tai/cordless_voice_poc.suprx")) has_poc = 1;
            else if (line_equals_trimmed(p, len, "ur0:tai/cordless_hud.suprx")) has_hud = 1;
        }
        p = nl ? nl + 1 : end;
    }
    free(txt);
    return has_mem && has_poc && has_hud;
}

static int patch_tai_config(int add_entries, char *errbuf, size_t errlen) {
    size_t sz = 0;
    char *orig = read_text_file(TAI_CONFIG, &sz);
    if (orig) {
        SceUID bfd = sceIoOpen(TAI_CONFIG_BAK, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (bfd < 0 || write_all(bfd, orig, sz) < 0) {
            if (bfd >= 0) sceIoClose(bfd);
            free(orig);
            snprintf(errbuf, errlen, "Failed to back up ur0:tai/config.txt");
            return -1;
        }
        sceIoClose(bfd);
    }

    const char *eol = (orig && memchr(orig, '\r', sz)) ? "\r\n" : "\n";
    size_t eol_len = strlen(eol);

    SceUID wfd = sceIoOpen(TAI_CONFIG_TMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (wfd < 0) {
        free(orig);
        snprintf(errbuf, errlen, "Failed to open ur0:tai/config.txt.tmp");
        return -1;
    }

    int wrote_kernel = 0, wrote_main = 0, wrote_all = 0;
    int wr_err = 0;
    if (orig) {
        const char *p = orig, *end = orig + sz;
        while (p < end && !wr_err) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
            size_t clean_len = (len > 0 && p[len - 1] == '\r') ? len - 1 : len;

            if (!is_cordless_line(p, clean_len)) {
                if (clean_len > 0 && write_all(wfd, p, clean_len) < 0) wr_err = 1;
                if (!wr_err && write_all(wfd, eol, eol_len) < 0) wr_err = 1;

                if (!wr_err && add_entries) {
                    const char *ins = NULL;
                    if (!wrote_kernel && line_equals_trimmed(p, clean_len, "*KERNEL")) {
                        ins = "ur0:tai/cordless_mem.skprx"; wrote_kernel = 1;
                    } else if (!wrote_main && line_equals_trimmed(p, clean_len, "*main")) {
                        ins = "ur0:tai/cordless_voice_poc.suprx"; wrote_main = 1;
                    } else if (!wrote_all && line_equals_trimmed(p, clean_len, "*ALL")) {
                        ins = "ur0:tai/cordless_hud.suprx"; wrote_all = 1;
                    }
                    if (ins) {
                        if (write_all(wfd, ins, strlen(ins)) < 0 ||
                            write_all(wfd, eol, eol_len) < 0) wr_err = 1;
                    }
                }
            }
            p = nl ? nl + 1 : end;
        }
        free(orig);
    }

    if (!wr_err && add_entries) {
        struct { int done; const char *sec; const char *line; } tail[3] = {
            { wrote_kernel, "*KERNEL", "ur0:tai/cordless_mem.skprx" },
            { wrote_main,   "*main",   "ur0:tai/cordless_voice_poc.suprx" },
            { wrote_all,    "*ALL",    "ur0:tai/cordless_hud.suprx" },
        };
        for (int i = 0; i < 3 && !wr_err; i++) {
            if (tail[i].done) continue;
            if (write_all(wfd, tail[i].sec, strlen(tail[i].sec)) < 0 ||
                write_all(wfd, eol, eol_len) < 0 ||
                write_all(wfd, tail[i].line, strlen(tail[i].line)) < 0 ||
                write_all(wfd, eol, eol_len) < 0) wr_err = 1;
        }
    }

    sceIoClose(wfd);
    if (wr_err) {
        sceIoRemove(TAI_CONFIG_TMP);
        snprintf(errbuf, errlen, "Failed writing ur0:tai/config.txt.tmp");
        return -1;
    }

    sceIoRemove(TAI_CONFIG);
    if (sceIoRename(TAI_CONFIG_TMP, TAI_CONFIG) < 0) {
        snprintf(errbuf, errlen, "Failed to replace ur0:tai/config.txt");
        return -1;
    }
    return 0;
}

void plugin_get_status(PluginStatus *out) {
    memset(out, 0, sizeof(*out));
    int bundled = 1, installed = 1;
    for (int i = 0; i < 4; i++) {
        char path[128];
        if (!find_plugin_src(k_plugin_files[i], path, sizeof(path))) bundled = 0;
        snprintf(path, sizeof(path), TAI_DIR "/%s", k_plugin_files[i]);
        if (!file_exists(path)) installed = 0;
    }
    out->bundled = bundled;
    out->files_installed = installed;
    out->config_installed = config_has_all_entries();
    out->core_active = voice_ipc_available() ? 1 : 0;
}

int plugin_install(char *errbuf, size_t errlen) {
    sceIoMkdir(TAI_DIR, 0777);
    for (int i = 0; i < 4; i++) {
        char src[128], dst[128];
        if (!find_plugin_src(k_plugin_files[i], src, sizeof(src))) {
            snprintf(errbuf, errlen, "Missing %s in VPK or ux0:data/", k_plugin_files[i]);
            return -1;
        }
        snprintf(dst, sizeof(dst), TAI_DIR "/%s", k_plugin_files[i]);
        if (copy_file(src, dst) < 0) {
            snprintf(errbuf, errlen, "Failed to copy %s", k_plugin_files[i]);
            return -1;
        }
    }
    if (patch_tai_config(1, errbuf, errlen) < 0) return -1;
    console_logf("[plugin-install] installed 4 modules + patched ur0:tai/config.txt");
    return 0;
}

int plugin_uninstall(char *errbuf, size_t errlen) {
    if (patch_tai_config(0, errbuf, errlen) < 0) return -1;
    for (int i = 0; i < 4; i++) {
        char dst[128];
        snprintf(dst, sizeof(dst), TAI_DIR "/%s", k_plugin_files[i]);
        sceIoRemove(dst);
    }
    console_logf("[plugin-install] uninstalled plugin modules + cleaned ur0:tai/config.txt");
    return 0;
}

void plugin_request_reboot(void) {
    scePowerRequestColdReset();
}
