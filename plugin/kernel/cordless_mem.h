#ifndef CORDLESS_MEM_H
#define CORDLESS_MEM_H

#include <psp2/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CORDLESS_HUD_MAX_PEERS 5

#define CORDLESS_OV_ACTIVE      (1u << 0)
#define CORDLESS_OV_CONNECTED   (1u << 1)
#define CORDLESS_OV_MUTED       (1u << 2)
#define CORDLESS_OV_DEAF        (1u << 3)
#define CORDLESS_OV_TX_SPEAKING (1u << 4)

#define CORDLESS_PEER_SPEAKING  (1u << 0)
#define CORDLESS_PEER_MUTED     (1u << 1)
#define CORDLESS_PEER_DEAF      (1u << 2)

#define CORDLESS_ACT_MUTE       (1u << 0)
#define CORDLESS_ACT_DEAF       (1u << 1)
#define CORDLESS_ACT_LEAVE      (1u << 2)

typedef struct cordless_overlay_peer {
    char    name[20];
    uint8_t flags;
    uint8_t pad[3];
} cordless_overlay_peer;

typedef struct cordless_overlay_state {
    uint32_t              flags;
    char                  channel[28];
    uint8_t               peer_count;
    uint8_t               game_vol_pct;
    uint8_t               pad[2];
    cordless_overlay_peer peers[CORDLESS_HUD_MAX_PEERS];
} cordless_overlay_state;

typedef struct cordless_mem_probe_entry {
    uint32_t type;
    int32_t  rc;
    uint32_t addr;
} cordless_mem_probe_entry;

SceUID   cordless_mem_alloc_pool(uint32_t size, uint32_t *out_addr, uint32_t *out_type);
int      cordless_mem_free_pool(SceUID uid);
int      cordless_mem_probe(uint32_t size, cordless_mem_probe_entry *out, int max_entries);
int      cordless_mem_set_overlay(int active, int connected, int muted, int deaf);
int      cordless_mem_get_overlay(void);
int      cordless_mem_set_overlay_state(const cordless_overlay_state *state);
int      cordless_mem_get_overlay_state(cordless_overlay_state *out);
int      cordless_mem_post_action(uint32_t mask);
uint32_t cordless_mem_take_action(void);
int      cordless_mem_set_game_vol(int pct);
int      cordless_mem_get_game_vol(void);

#ifdef __cplusplus
}
#endif

#endif /* CORDLESS_MEM_H */
