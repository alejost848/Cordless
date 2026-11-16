#include "../kernel/cordless_mem.h"

#include <psp2/kernel/threadmgr.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Thread-safe free-list allocator backed by a kernel-allocated
 * USER_SHARED_MAIN_RW memory block (so SceShell's 1 MiB USER_RW is untouched).
 * Hooked via -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
 * and -Wl,--wrap=_malloc_r,--wrap=_free_r,--wrap=_calloc_r,--wrap=_realloc_r.
 */

#define POOL_SIZE (3 * 1024 * 1024)
#define ALIGN8(x) (((x) + 7u) & ~7u)

typedef struct block_hdr {
    uint32_t size; /* payload size in bytes (8-byte aligned) */
    uint32_t free; /* 1 = free, 0 = used */
    struct block_hdr *next;
} block_hdr_t;

static SceUID s_pool_uid = -1;
static block_hdr_t *s_head = NULL;
static SceKernelLwMutexWork s_mtx;
static int s_mtx_init = 0;

static void pool_lock(void) {
    if (!s_mtx_init) {
        sceKernelCreateLwMutex(&s_mtx, "cl_pool_mtx", 0, 0, NULL);
        s_mtx_init = 1;
    }
    sceKernelLockLwMutex(&s_mtx, 1, NULL);
}

static void pool_unlock(void) {
    sceKernelUnlockLwMutex(&s_mtx, 1);
}

extern void console_logf(const char *fmt, ...);

static int pool_ensure_init(void) {
    if (s_head) return 0;
    static const uint32_t sizes[] = {
        3 * 1024 * 1024,
        2 * 1024 * 1024,
        1536 * 1024,
        1024 * 1024,
    };
    uint32_t addr = 0, type = 0, chosen = 0;
    for (int i = 0; i < 4; i++) {
        s_pool_uid = cordless_mem_alloc_pool(sizes[i], &addr, &type);
        console_logf("[pool] try(%u KiB) uid=0x%08x addr=0x%08x type=0x%08x",
                     (unsigned)(sizes[i] / 1024), (unsigned)s_pool_uid, addr, type);
        if (s_pool_uid >= 0 && addr != 0) {
            chosen = sizes[i];
            break;
        }
    }
    if (s_pool_uid < 0 || addr == 0) return -1;

    s_head = (block_hdr_t *)(uintptr_t)addr;
    s_head->size = chosen - sizeof(block_hdr_t);
    s_head->free = 1;
    s_head->next = NULL;
    return 0;
}

void pool_alloc_shutdown(void) {
    pool_lock();
    if (s_pool_uid >= 0) {
        cordless_mem_free_pool(s_pool_uid);
        s_pool_uid = -1;
    }
    s_head = NULL;
    pool_unlock();
}

static void *pool_malloc_locked(size_t size) {
    if (size == 0) size = 8;
    size = ALIGN8(size);
    if (pool_ensure_init() < 0) return NULL;

    for (block_hdr_t *b = s_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            if (b->size >= size + sizeof(block_hdr_t) + 16) {
                block_hdr_t *nb = (block_hdr_t *)((uint8_t *)(b + 1) + size);
                nb->size = b->size - (uint32_t)size - (uint32_t)sizeof(block_hdr_t);
                nb->free = 1;
                nb->next = b->next;
                b->size = (uint32_t)size;
                b->next = nb;
            }
            b->free = 0;
            return (void *)(b + 1);
        }
    }
    return NULL;
}

static void pool_free_locked(void *ptr) {
    if (!ptr || !s_head) return;
    block_hdr_t *b = ((block_hdr_t *)ptr) - 1;
    b->free = 1;

    /* Coalesce adjacent free blocks */
    for (block_hdr_t *cur = s_head; cur && cur->next;) {
        if (cur->free && cur->next->free) {
            cur->size += (uint32_t)sizeof(block_hdr_t) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

void *__wrap_malloc(size_t size) {
    pool_lock();
    void *p = pool_malloc_locked(size);
    pool_unlock();
    return p;
}

void __wrap_free(void *ptr) {
    if (!ptr) return;
    pool_lock();
    pool_free_locked(ptr);
    pool_unlock();
}

void *__wrap_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = __wrap_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *__wrap_realloc(void *ptr, size_t size) {
    if (!ptr) return __wrap_malloc(size);
    if (size == 0) {
        __wrap_free(ptr);
        return NULL;
    }
    block_hdr_t *b = ((block_hdr_t *)ptr) - 1;
    if (b->size >= size) return ptr;

    void *np = __wrap_malloc(size);
    if (np) {
        memcpy(np, ptr, b->size);
        __wrap_free(ptr);
    }
    return np;
}

void *__wrap__malloc_r(struct _reent *r, size_t size) {
    (void)r;
    return __wrap_malloc(size);
}

void __wrap__free_r(struct _reent *r, void *ptr) {
    (void)r;
    __wrap_free(ptr);
}

void *__wrap__calloc_r(struct _reent *r, size_t nmemb, size_t size) {
    (void)r;
    return __wrap_calloc(nmemb, size);
}

void *__wrap__realloc_r(struct _reent *r, void *ptr, size_t size) {
    (void)r;
    return __wrap_realloc(ptr, size);
}
