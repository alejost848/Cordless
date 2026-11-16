#include "cordless_mem.h"

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr/misc.h>

#include <stdint.h>

static SceUID s_pool_uid = -1;

SceUID cordless_mem_alloc_pool(uint32_t size, uint32_t *out_addr, uint32_t *out_type) {
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
