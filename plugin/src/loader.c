#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>

#include "../taihen/taihen.h"

#include <stdarg.h>
#include <stdint.h>

#define LOG_PATH "ur0:data/cordless_voice_poc.log"
#define CORE_PATH "ur0:tai/cordless_voice_core.suprx"
#define VOICE_WAKE_PORT 47322
#define SCENET_LIB_NID  0x6BF8B2A2u

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

typedef int (*pfn_net_init_t)(SceNetInitParam *param);
typedef int (*pfn_net_socket_t)(const char *name, int domain, int type, int protocol);
typedef int (*pfn_net_bind_t)(int s, const SceNetSockaddr *addr, unsigned int addrlen);
typedef int (*pfn_net_listen_t)(int s, int backlog);
typedef int (*pfn_net_accept_t)(int s, SceNetSockaddr *addr, unsigned int *addrlen);
typedef int (*pfn_net_setsockopt_t)(int s, int level, int optname,
                                    const void *optval, unsigned int optlen);
typedef int (*pfn_net_send_t)(int s, const void *msg, unsigned int len, int flags);
typedef int (*pfn_net_close_t)(int s);
typedef int *(*pfn_net_errno_t)(void);

static pfn_net_init_t       p_net_init = NULL;
static pfn_net_socket_t     p_net_socket = NULL;
static pfn_net_bind_t       p_net_bind = NULL;
static pfn_net_listen_t     p_net_listen = NULL;
static pfn_net_accept_t     p_net_accept = NULL;
static pfn_net_setsockopt_t p_net_setsockopt = NULL;
static pfn_net_send_t       p_net_send = NULL;
static pfn_net_close_t      p_net_close = NULL;
static pfn_net_errno_t      p_net_errno = NULL;
static int s_net_resolved = 0;
static int s_net_init_tried = 0;
static int s_wake_fd = -1;

static inline uint16_t net_htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t net_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

static int resolve_one(uint32_t nid, uintptr_t *out) {
    int rc = taiGetModuleExportFunc("SceNet", SCENET_LIB_NID, nid, out);
    if (rc < 0 || !*out) {
        ldr_log("taiGetModuleExportFunc(SceNet, 0x%08x) = 0x%08x", nid, rc);
        return 0;
    }
    return 1;
}

static int resolve_net_funcs(void) {
    if (s_net_resolved) return 1;
    uintptr_t fn = 0;
    if (!resolve_one(0xEB03E265u, &fn)) return 0;
    p_net_init = (pfn_net_init_t)fn;
    if (!resolve_one(0xF084FCE3u, &fn)) return 0;
    p_net_socket = (pfn_net_socket_t)fn;
    if (!resolve_one(0x1296A94Bu, &fn)) return 0;
    p_net_bind = (pfn_net_bind_t)fn;
    if (!resolve_one(0x7A8DA094u, &fn)) return 0;
    p_net_listen = (pfn_net_listen_t)fn;
    if (!resolve_one(0x1ADF9BB1u, &fn)) return 0;
    p_net_accept = (pfn_net_accept_t)fn;
    if (!resolve_one(0x065505CAu, &fn)) return 0;
    p_net_setsockopt = (pfn_net_setsockopt_t)fn;
    if (!resolve_one(0xE3DD8CD9u, &fn)) return 0;
    p_net_send = (pfn_net_send_t)fn;
    if (!resolve_one(0x29822B4Du, &fn)) return 0;
    p_net_close = (pfn_net_close_t)fn;
    if (!resolve_one(0xE37F34AAu, &fn)) return 0;
    p_net_errno = (pfn_net_errno_t)fn;
    s_net_resolved = 1;
    ldr_log("SceNet exports resolved dynamically");
    return 1;
}

static void ensure_net_init_if_needed(void) {
    if (s_net_init_tried) return;
    s_net_init_tried = 1;
    SceUID blk = sceKernelAllocMemBlock("cordless_net",
                                        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                        0x24000, NULL);
    if (blk < 0) {
        ldr_log("fallback alloc memblock failed: 0x%08x", blk);
        return;
    }
    void *mem = NULL;
    if (sceKernelGetMemBlockBase(blk, &mem) < 0 || !mem) {
        sceKernelFreeMemBlock(blk);
        return;
    }
    SceNetInitParam param;
    sceClibMemset(&param, 0, sizeof(param));
    param.memory = mem;
    param.size = 0x24000;
    param.flags = 0;
    int rc = p_net_init(&param);
    ldr_log("fallback sceNetInit(144K) = 0x%08x", rc);
    if (rc != 0)
        sceKernelFreeMemBlock(blk);
}

static int try_open_wake_socket(int allow_init_fallback) {
    if (s_wake_fd >= 0) return 0;
    if (!resolve_net_funcs()) return -1;

    int fd = p_net_socket("cordless_wake", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0 && allow_init_fallback && !s_net_init_tried) {
        ldr_log("wake socket initial attempt failed (%d, err=%d)",
                fd, p_net_errno ? *p_net_errno() : -1);
        ensure_net_init_if_needed();
        fd = p_net_socket("cordless_wake", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    }
    if (fd < 0) return -1;

    int yes = 1;
    p_net_setsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &yes, sizeof(yes));
    int nbio = 1;
    p_net_setsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nbio, sizeof(nbio));

    SceNetSockaddrIn sa;
    sceClibMemset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port = net_htons(VOICE_WAKE_PORT);
    sa.sin_addr.s_addr = net_htonl(SCE_NET_INADDR_LOOPBACK);

    if (p_net_bind(fd, (const SceNetSockaddr *)&sa, sizeof(sa)) < 0 ||
        p_net_listen(fd, 2) < 0) {
        ldr_log("wake bind/listen failed (err=%d)", p_net_errno ? *p_net_errno() : -1);
        p_net_close(fd);
        return -1;
    }

    s_wake_fd = fd;
    ldr_log("wake listener ready on 127.0.0.1:%d (fd=%d)", VOICE_WAKE_PORT, fd);
    return 0;
}

static void poll_wake_socket(void) {
    if (s_wake_fd < 0) return;
    int cfd = p_net_accept(s_wake_fd, NULL, NULL);
    if (cfd < 0) return;
    ldr_log("wake connection accepted (core=%d)", s_core_mod);
    if (s_core_mod < 0)
        load_core();
    const char *reply = (s_core_mod >= 0) ? "ok\n" : "err\n";
    unsigned int rlen = (s_core_mod >= 0) ? 3u : 4u;
    p_net_send(cfd, reply, rlen, 0);
    p_net_close(cfd);
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
    try_open_wake_socket(0);

    /* Wait for L + R + START (manual load/reload) OR TCP wake on 127.0.0.1:47322
     * when cordless.vpk requests on-demand load (keeps boot untouched). */
    int combo_held = 0;
    int wake_retry_ticks = 0;
    const uint32_t reload_combo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
    while (s_running) {
        if (s_wake_fd < 0) {
            wake_retry_ticks++;
            if ((!s_net_init_tried && (wake_retry_ticks % 20) == 0) ||
                (wake_retry_ticks % 100) == 0) {
                try_open_wake_socket(wake_retry_ticks >= 40);
            }
        } else {
            poll_wake_socket();
        }

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

    if (s_wake_fd >= 0 && p_net_close) {
        p_net_close(s_wake_fd);
        s_wake_fd = -1;
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
