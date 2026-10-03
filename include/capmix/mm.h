#ifndef CAPMIX_MM_H
#define CAPMIX_MM_H

#include "types.h"

/* Layer 1 mix:
 *   buddy + SLUB          — Linux
 *   no-overcommit         — Windows
 *   8-state page machine  — Windows
 *   4-level pressure pipe — macOS (file discard / compress / compressed-swap / jetsam)
 *   OOM killer fallback   — Linux
 *   watermarks + kswapd   — Linux
 *   THP = madvise only    — Linux lesson
 */

typedef enum {
    CM_PAGE_ACTIVE = 0,
    CM_PAGE_TRANSITION,
    CM_PAGE_STANDBY,
    CM_PAGE_MODIFIED,
    CM_PAGE_MODIFIED_NOWRITE,
    CM_PAGE_FREE,
    CM_PAGE_ZEROED,
    CM_PAGE_BAD,
    CM_PAGE_NSTATES
} cm_page_state_t;

typedef enum {
    CM_PRESS_IDLE = 0,
    CM_PRESS_L1_DISCARD,
    CM_PRESS_L2_COMPRESS,
    CM_PRESS_L3_COMPSWAP,
    CM_PRESS_L4_JETSAM,
} cm_pressure_level_t;

typedef struct {
    cm_paddr_t base;
    uint64_t   npages;
    uint32_t   type;
} cm_mem_region_t;

typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t committed_pages;
    uint64_t commit_limit;
    uint64_t slab_pages;
    uint64_t compressed_pages;
    uint64_t file_cache_pages;
    uint64_t anon_pages;
    uint64_t swap_slots;
    uint32_t jetsam_kills;
    cm_pressure_level_t pressure;
} cm_mm_stats_t;

void      cm_mm_init(const cm_mem_region_t *regions, int nregions);
void     *cm_page_alloc(unsigned order);
void      cm_page_free(void *ptr, unsigned order);
void     *cm_kmalloc(size_t size);
void      cm_kfree(void *ptr);

cm_error_t cm_commit_charge(uint64_t pages);
void       cm_commit_uncharge(uint64_t pages);

/* Page cache / anon — reclaim sources for the 4-level pipe. */
cm_error_t cm_cache_add_file(cm_id_t ino, uint64_t off, void *page, int dirty);
uint64_t   cm_cache_discard_clean(uint64_t want);
cm_error_t cm_anon_track(void *page);
uint64_t   cm_anon_compress(uint64_t want);
uint64_t   cm_comp_swapout(uint64_t want);

cm_pressure_level_t cm_pressure_tick(void);
uint64_t            cm_pressure_reclaim(uint64_t want_free);
void                cm_mm_stats(cm_mm_stats_t *out);
const char         *cm_page_state_name(cm_page_state_t s);
const char         *cm_pressure_name(cm_pressure_level_t l);
void                cm_mm_dump(void);

uint64_t cm_compressed_pages(void);
uint64_t cm_file_cache_pages(void);
uint64_t cm_anon_pages(void);
uint64_t cm_swap_slots(void);
uint32_t cm_jetsam_kills(void);
void     cm_pressure_note_jetsam(void);

#endif
