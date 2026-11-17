#include "cordless_mem.h"

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr/misc.h>

#include <stdint.h>

static SceUID s_pool_uid = -1;
static volatile int s_ov_active = 0;
static volatile int s_ov_conn   = 0;
static volatile int s_ov_muted  = 0;
static volatile int s_ov_deaf   = 0;
static cordless_overlay_state s_ov_state;
static volatile uint32_t s_pending_actions = 0;
static volatile int s_game_vol = 100;

int cordless_mem_set_overlay(int active, int connected, int muted, int deaf) {
    uint32_t state;
    ENTER_SYSCALL(state);
    s_ov_active = active ? 1 : 0;
    s_ov_conn   = connected ? 1 : 0;
    s_ov_muted  = muted ? 1 : 0;
    s_ov_deaf   = deaf ? 1 : 0;
    s_ov_state.flags = (s_ov_active ? CORDLESS_OV_ACTIVE : 0) |
                       (s_ov_conn   ? CORDLESS_OV_CONNECTED : 0) |
                       (s_ov_muted  ? CORDLESS_OV_MUTED : 0) |
                       (s_ov_deaf   ? CORDLESS_OV_DEAF : 0);
    if (!s_ov_active) {
        memset(&s_ov_state, 0, sizeof(s_ov_state));
        s_pending_actions = 0;
    }
    EXIT_SYSCALL(state);
    return 0;
}

int cordless_mem_get_overlay(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int r = (s_ov_active ? 1 : 0) |
            (s_ov_conn   ? 2 : 0) |
            (s_ov_muted  ? 4 : 0) |
            (s_ov_deaf   ? 8 : 0);
    EXIT_SYSCALL(state);
    return r;
}

int cordless_mem_set_overlay_state(const cordless_overlay_state *in) {
    uint32_t state;
    ENTER_SYSCALL(state);
    if (!in) {
        EXIT_SYSCALL(state);
        return -1;
    }
    cordless_overlay_state tmp;
    int rc = ksceKernelMemcpyUserToKernel(&tmp, in, sizeof(tmp));
    if (rc < 0) {
        EXIT_SYSCALL(state);
        return rc;
    }
    if (tmp.peer_count > CORDLESS_HUD_MAX_PEERS)
        tmp.peer_count = CORDLESS_HUD_MAX_PEERS;
    tmp.channel[sizeof(tmp.channel) - 1] = '\0';
    for (int i = 0; i < CORDLESS_HUD_MAX_PEERS; i++)
        tmp.peers[i].name[sizeof(tmp.peers[i].name) - 1] = '\0';
    tmp.game_vol_pct = (uint8_t)s_game_vol;
    s_ov_state  = tmp;
    s_ov_active = (tmp.flags & CORDLESS_OV_ACTIVE) ? 1 : 0;
    s_ov_conn   = (tmp.flags & CORDLESS_OV_CONNECTED) ? 1 : 0;
    s_ov_muted  = (tmp.flags & CORDLESS_OV_MUTED) ? 1 : 0;
    s_ov_deaf   = (tmp.flags & CORDLESS_OV_DEAF) ? 1 : 0;
    if (!s_ov_active) s_pending_actions = 0;
    EXIT_SYSCALL(state);
    return 0;
}

int cordless_mem_get_overlay_state(cordless_overlay_state *out) {
    uint32_t state;
    ENTER_SYSCALL(state);
    if (!out) {
        EXIT_SYSCALL(state);
        return -1;
    }
    cordless_overlay_state tmp = s_ov_state;
    int rc = ksceKernelMemcpyKernelToUser(out, &tmp, sizeof(tmp));
    EXIT_SYSCALL(state);
    return rc;
}

int cordless_mem_post_action(uint32_t mask) {
    uint32_t state;
    ENTER_SYSCALL(state);
    s_pending_actions |= mask;
    EXIT_SYSCALL(state);
    return 0;
}

uint32_t cordless_mem_take_action(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    uint32_t act = s_pending_actions;
    s_pending_actions = 0;
    EXIT_SYSCALL(state);
    return act;
}

int cordless_mem_set_game_vol(int pct) {
    uint32_t state;
    ENTER_SYSCALL(state);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_game_vol = pct;
    s_ov_state.game_vol_pct = (uint8_t)pct;
    EXIT_SYSCALL(state);
    return 0;
}

int cordless_mem_get_game_vol(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int v = s_game_vol;
    EXIT_SYSCALL(state);
    return v;
}

SceUID cordless_mem_alloc_pool(uint32_t size, uint32_t *out_addr, uint32_t *out_type) {
    uint32_t state;
    ENTER_SYSCALL(state);

    if (s_pool_uid >= 0) {
        ksceKernelFreeMemBlock(s_pool_uid);
        s_pool_uid = -1;
    }

    if (size == 0 || (size & 0xFFF) != 0) {
        size = (size + 0xFFF) & ~0xFFFu;
    }

    const SceKernelMemBlockType candidates[] = {
        SCE_KERNEL_MEMBLOCK_TYPE_USER_SHARED_MAIN_RW,
    };

    SceUID uid = -1;
    uint32_t chosen_type = 0;
    for (int i = 0; i < 1; i++) {
        uid = ksceKernelAllocMemBlock("cordless_pool", candidates[i], size, NULL);
        if (uid >= 0) {
            chosen_type = (uint32_t)candidates[i];
            break;
        }
    }

    if (uid >= 0) {
        void *base = NULL;
        int rc = ksceKernelGetMemBlockBase(uid, &base);
        if (rc < 0 || !base) {
            ksceKernelFreeMemBlock(uid);
            uid = rc < 0 ? rc : -1;
        } else {
            uint32_t addr = (uint32_t)(uintptr_t)base;
            if (out_addr) ksceKernelMemcpyKernelToUser(out_addr, &addr, sizeof(addr));
            if (out_type) ksceKernelMemcpyKernelToUser(out_type, &chosen_type, sizeof(chosen_type));
            s_pool_uid = uid;
        }
    }

    EXIT_SYSCALL(state);
    return uid;
}

int cordless_mem_free_pool(SceUID uid) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int rc = ksceKernelFreeMemBlock(uid);
    if (uid == s_pool_uid) s_pool_uid = -1;
    EXIT_SYSCALL(state);
    return rc;
}

int cordless_mem_probe(uint32_t size, cordless_mem_probe_entry *out, int max_entries) {
    uint32_t state;
    ENTER_SYSCALL(state);

    if (size == 0 || (size & 0xFFF) != 0) {
        size = (size + 0xFFF) & ~0xFFFu;
    }

    SceUID pid = ksceKernelGetProcessId();
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_PID;
    opt.pid = pid;

    static const struct {
        SceKernelMemBlockType type;
        int use_pid;
    } tests[] = {
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW,  1 },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW,  1 },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_RW,       1 },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_TOOL_RW,     1 },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_SHARED_CDIALOG_RW,0 },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_SHARED_MAIN_RW,   0 },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_SHARED_TOOL_RW,   0 },
    };

    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    if (n > max_entries) n = max_entries;

    for (int i = 0; i < n; i++) {
        cordless_mem_probe_entry e;
        e.type = (uint32_t)tests[i].type;
        e.addr = 0;
        SceUID uid = ksceKernelAllocMemBlock("cl_probe", tests[i].type, size,
                                             tests[i].use_pid ? &opt : NULL);
        e.rc = uid;
        if (uid >= 0) {
            void *base = NULL;
            if (ksceKernelGetMemBlockBase(uid, &base) == 0 && base) {
                e.addr = (uint32_t)(uintptr_t)base;
            }
            ksceKernelFreeMemBlock(uid);
        }
        if (out) {
            ksceKernelMemcpyKernelToUser(&out[i], &e, sizeof(e));
        }
    }

    EXIT_SYSCALL(state);
    return n;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    if (s_pool_uid >= 0) {
        ksceKernelFreeMemBlock(s_pool_uid);
        s_pool_uid = -1;
    }
    return SCE_KERNEL_STOP_SUCCESS;
}
