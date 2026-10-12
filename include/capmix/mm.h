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
    uint64_t compressed_pages;   /* anon pages held compressed in RAM */
    uint64_t pool_pages;         /* RAM the compressor pool occupies */
    uint64_t file_cache_pages;
    uint64_t anon_pages;         /* anon pages resident uncompressed */
    uint64_t swapped_pages;      /* anon pages whose compressed copy is on disk */
    uint64_t swap_slots;         /* 4 KiB swap segments in use */
    uint64_t swap_total;         /* 4 KiB swap segments on the device */
    uint32_t jetsam_kills;
    uint32_t oom_kills;
    cm_pressure_level_t pressure;
} cm_mm_stats_t;

typedef struct {
    uint64_t min, low, high;     /* free-page watermarks (Linux zone style) */
} cm_watermarks_t;

/* Per-stage counters of the pressure pipeline, cumulative since boot. */
typedef struct {
    uint64_t l1_discarded;       /* clean file pages dropped */
    uint64_t l2_compressed;      /* anon pages compressed */
    uint64_t l2_incompressible;  /* anon pages that did not compress */
    uint64_t l3_segments_out;    /* 4 KiB compressor segments written to swap */
    uint64_t l3_pages_out;       /* anon pages those segments carried */
    uint64_t swapins;            /* soft faults served from swap */
    uint64_t decompressions;     /* soft faults served from the pool */
    uint64_t direct_reclaims;    /* allocations that had to reclaim first */
    uint64_t alloc_failures;     /* allocations refused after L1..L4 */
    uint64_t lost_pages;         /* swap-in failed (I/O / checksum) */
    uint64_t l3_compactions;     /* sparse segments compacted in RAM, no I/O */
    uint64_t max_alloc_work;     /* most reclaim work units one allocation did */
    uint32_t jetsam_kills;
    uint32_t oom_kills;
    cm_pressure_level_t deepest; /* deepest stage ever used */
} cm_pressure_stats_t;

typedef struct {uint64_t base,pages,free_pages;unsigned node;} cm_buddy_arena_info_t;
cm_error_t cm_buddy_arena_snapshot(unsigned index,cm_buddy_arena_info_t *out);
int cm_buddy_smp_selftest(void);

void      cm_mm_init(const cm_mem_region_t *regions, int nregions);
void     *cm_page_alloc(unsigned order);
void      cm_page_free(void *ptr, unsigned order);
void     *cm_kmalloc(size_t size);
void      cm_kfree(void *ptr);
uint64_t  cm_mm_free_pages(void);
uint64_t  cm_mm_total_pages(void);

cm_error_t cm_commit_charge(uint64_t pages);
void       cm_commit_uncharge(uint64_t pages);
void       cm_commit_add_backing(uint64_t pages);   /* swap extends the limit */

/* --- anonymous memory (mm/anon.c) ---------------------------------------
 * An anon page is owned by a process (pid) and lives in exactly one of:
 *   RESIDENT    a buddy page
 *   COMPRESSED  a WKdm blob in a compressor-pool page            (L2)
 *   SWAPPED     that blob inside a 4 KiB segment on the swap disk (L3)
 * cm_anon_access() is the soft fault: whatever state the page is in, it
 * comes back RESIDENT with its exact contents (or NULL if it was lost).
 * Every allocation charges commit (no-overcommit) and may direct-reclaim. */
typedef uint32_t cm_anon_id_t;               /* 0 = invalid */

void         cm_anon_init(void);
cm_anon_id_t cm_anon_alloc(cm_id_t owner);
void        *cm_anon_access(cm_anon_id_t id, int write);
void         cm_anon_free(cm_anon_id_t id);
uint64_t     cm_anon_free_owner(cm_id_t owner);
uint64_t     cm_anon_owner_pages(cm_id_t owner);  /* rss, all states */
uint64_t     cm_anon_compress(uint64_t want);     /* L2 */
uint64_t     cm_comp_swapout(uint64_t want);      /* L3 */

/* --- page cache (mm/cache.c), keyed by fs block number -----------------
 * COW blocks are immutable while referenced, so a block number is a
 * coherent cache key. Clean pages are the L1 reclaim source. */
void       cm_pcache_init(void);
int        cm_pcache_read(uint32_t blk, void *dst);         /* 1 = hit */
void       cm_pcache_fill(uint32_t blk, const void *src);   /* opportunistic */
void       cm_pcache_invalidate(uint32_t blk);
void       cm_pcache_invalidate_all(void);
uint64_t   cm_cache_discard_clean(uint64_t want);           /* L1 */
uint64_t   cm_pcache_hits(void);
uint64_t   cm_pcache_misses(void);

/* --- pressure pipeline (mm/pressure.c) --------------------------------- */
void                cm_pressure_init(void);
cm_pressure_level_t cm_pressure_tick(void);
uint64_t            cm_pressure_reclaim(uint64_t want_free);   /* L1..L3 */
int                 cm_mm_balance(int need_pages);  /* L1..L4 until min+need */
void               *cm_page_alloc_user(void);       /* watermark-aware */
void                cm_mm_watermarks(cm_watermarks_t *out);
void                cm_pressure_stats(cm_pressure_stats_t *out);
void                cm_pressure_reset_max(void);
void                cm_mm_stats(cm_mm_stats_t *out);
const char         *cm_page_state_name(cm_page_state_t s);
const char         *cm_pressure_name(cm_pressure_level_t l);
void                cm_mm_dump(void);
void                cm_mm_lock(void);
void                cm_mm_unlock(void);

uint64_t cm_compressed_pages(void);
uint64_t cm_pool_pages(void);
uint64_t cm_file_cache_pages(void);
uint64_t cm_anon_pages(void);
uint64_t cm_swapped_pages(void);
uint64_t cm_swap_slots(void);
uint64_t cm_swap_total(void);
uint32_t cm_jetsam_kills(void);
uint32_t cm_oom_kills(void);

#endif
