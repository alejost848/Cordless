#include <psp2/kernel/threadmgr.h>
#include <reent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Lightweight replacement for vita-newlib's lib_a-threading.o, which
 * statically reserves 270 KiB (reent_list) in .bss inside SceShell's USER_RW.
 * We only ever have <= 8 threads in cordless_voice_core.suprx.
 */

#define MAX_REENT_SLOTS 12

struct _reent _newlib_global_reent = _REENT_INIT(_newlib_global_reent);

typedef struct {
    SceUID tid;
    struct _reent reent;
    uint8_t pthread_data[256];
    uint8_t tls_data[256];
} reent_slot_t;

static reent_slot_t s_slots[MAX_REENT_SLOTS];
static SceKernelLwMutexWork s_reent_mtx;
static volatile int s_reent_state = 0;

static void reent_lock(void) {
    int st = __atomic_load_n(&s_reent_state, __ATOMIC_ACQUIRE);
    if (st != 2) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&s_reent_state, &expected, 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            sceKernelCreateLwMutex(&s_reent_mtx, "cl_reent", 0, 0, NULL);
            __atomic_store_n(&s_reent_state, 2, __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(&s_reent_state, __ATOMIC_ACQUIRE) != 2) {
                sceKernelDelayThread(100);
            }
        }
    }
    sceKernelLockLwMutex(&s_reent_mtx, 1, NULL);
}

static void reent_unlock(void) {
    sceKernelUnlockLwMutex(&s_reent_mtx, 1);
}

static reent_slot_t *find_or_alloc_slot(SceUID tid) {
    reent_lock();
    int free_idx = -1;
    for (int i = 0; i < MAX_REENT_SLOTS; i++) {
        if (s_slots[i].tid == tid) {
            reent_unlock();
            return &s_slots[i];
        }
        if (free_idx < 0 && s_slots[i].tid == 0) free_idx = i;
    }
    if (free_idx >= 0) {
        memset(&s_slots[free_idx], 0, sizeof(s_slots[free_idx]));
        s_slots[free_idx].tid = tid;
        _REENT_INIT_PTR(&s_slots[free_idx].reent);
        reent_unlock();
        return &s_slots[free_idx];
    }
    reent_unlock();
    return NULL;
}

void _init_vita_reent(void) {
    _REENT_INIT_PTR(&_newlib_global_reent);
}

void _free_vita_reent(void) {}

struct _reent *__getreent_for_thread(SceUID tid) {
    if (tid <= 0) return &_newlib_global_reent;
    reent_slot_t *s = find_or_alloc_slot(tid);
    return s ? &s->reent : &_newlib_global_reent;
}

struct _reent *__getreent(void) {
    return __getreent_for_thread(sceKernelGetThreadId());
}

void __vita_delete_thread_reent(SceUID tid) {
    reent_lock();
    for (int i = 0; i < MAX_REENT_SLOTS; i++) {
        if (s_slots[i].tid == tid) {
            _reclaim_reent(&s_slots[i].reent);
            s_slots[i].tid = 0;
            memset(s_slots[i].pthread_data, 0, sizeof(s_slots[i].pthread_data));
            memset(s_slots[i].tls_data, 0, sizeof(s_slots[i].tls_data));
            break;
        }
    }
    reent_unlock();
}

void vitasdk_delete_thread_reent(SceUID tid) {
    __vita_delete_thread_reent(tid);
}

void *vitasdk_get_pthread_data(SceUID thid) {
    if (thid <= 0) thid = sceKernelGetThreadId();
    reent_slot_t *s = find_or_alloc_slot(thid);
    return s ? (void *)s->pthread_data : NULL;
}

void *vitasdk_get_tls_data(SceUID thid) {
    if (thid <= 0) thid = sceKernelGetThreadId();
    reent_slot_t *s = find_or_alloc_slot(thid);
    return s ? (void *)s->tls_data : NULL;
}

void _exit_thread_common(int status, int delete_thr) {
    __vita_delete_thread_reent(sceKernelGetThreadId());
    if (delete_thr) sceKernelExitDeleteThread(status);
    else sceKernelExitThread(status);
}

void vita_exit_thread(int status)        { _exit_thread_common(status, 0); }
void vita_exit_delete_thread(int status) { _exit_thread_common(status, 1); }
