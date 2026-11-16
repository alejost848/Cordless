#ifndef CORDLESS_MEM_H
#define CORDLESS_MEM_H

#include <psp2/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cordless_mem_probe_entry {
    uint32_t type;
    int32_t  rc;
    uint32_t addr;
} cordless_mem_probe_entry;

SceUID cordless_mem_alloc_pool(uint32_t size, uint32_t *out_addr, uint32_t *out_type);
int    cordless_mem_free_pool(SceUID uid);
int    cordless_mem_probe(uint32_t size, cordless_mem_probe_entry *out, int max_entries);
int    cordless_mem_set_overlay(int active, int connected, int muted, int deaf);
int    cordless_mem_get_overlay(void);

#ifdef __cplusplus
}
#endif

#endif /* CORDLESS_MEM_H */
