#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include <stdarg.h>
#include <stdint.h>

#define LOG_PATH "ur0:data/cordless_voice_poc.log"
#define CORE_PATH "ur0:tai/cordless_voice_core.suprx"

static volatile int s_running = 0;
static SceUID s_thread_id = -1;
static SceUID s_core_mod = -1;

static void ldr_log(const char *fmt, ...) {
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

static void load_core(void) {
    int status = 0;
    s_core_mod = sceKernelLoadStartModule(CORE_PATH, 0, NULL, 0, NULL, &status);
    ldr_log("sceKernelLoadStartModule(%s) = 0x%08x (status=0x%08x)",
            CORE_PATH, s_core_mod, status);
}

static void unload_core(void) {
    if (s_core_mod >= 0) {
        int status = 0;
        int rc = sceKernelStopUnloadModule(s_core_mod, 0, NULL, 0, NULL, &status);
        ldr_log("sceKernelStopUnloadModule(0x%08x) = 0x%08x (status=0x%08x)",
                s_core_mod, rc, status);
        s_core_mod = -1;
    }
}

static int loader_thread(SceSize args, void *argp) {
    sceIoMkdir("ur0:data", 0777);
    ldr_log("=== cordless_voice_poc loader started ===");

    /* Wait 3s for SceShell to finish booting */
    sceKernelDelayThread(3 * 1000 * 1000);

    int sys_rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    ldr_log("sceSysmoduleLoadModule(NET) = 0x%08x", sys_rc);

    /* Wait for L + R + START to load/reload cordless_voice_core.suprx
     * (keeps boot & VPK installs 100% untouched). */
    int combo_held = 0;
    const uint32_t reload_combo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
    while (s_running) {
        SceCtrlData pad;
        sceClibMemset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);

        if ((pad.buttons & reload_combo) == reload_combo) {
            if (!combo_held) {
                combo_held = 1;
                ldr_log("L+R+START triggered (core=%d)!", s_core_mod);
                unload_core();
                sceKernelDelayThread(100 * 1000);
                load_core();
            }
        } else {
            combo_held = 0;
        }
        sceKernelDelayThread(50 * 1000);
    }

    unload_core();
    return sceKernelExitDeleteThread(0);
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    s_running = 1;
    s_thread_id = sceKernelCreateThread("cordless_ldr_thr", loader_thread,
                                      0x10000100, 0x2000, 0, 0, NULL);
    if (s_thread_id >= 0) {
        sceKernelStartThread(s_thread_id, 0, NULL);
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    s_running = 0;
    if (s_thread_id >= 0) {
        sceKernelWaitThreadEnd(s_thread_id, NULL, NULL);
        s_thread_id = -1;
    }
    unload_core();
    return SCE_KERNEL_STOP_SUCCESS;
}
