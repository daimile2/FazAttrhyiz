#include "capmix/mm.h"
#include "capmix/config.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

#ifdef CAPMIX_HOSTED
#include <stdlib.h>
#endif

typedef struct cm_free_block {
    struct cm_free_block *next;
} cm_free_block_t;

static cm_free_block_t *g_free[CM_BUDDY_MAX_ORDER + 1];
static uint64_t         g_free_count[CM_BUDDY_MAX_ORDER + 1];
static uint64_t         g_total_pages;
static uint64_t         g_free_pages;
static uint8_t         *g_heap;
static uint64_t         g_heap_pages;
static uint8_t         *g_page_state; /* one byte per page */

#ifdef CAPMIX_HOSTED
static uint8_t *g_sim_heap;
#endif

static const char *state_names[] = {
    "Active", "Transition", "Standby", "Modified",
    "ModNoWrite", "Free", "Zeroed", "Bad"
};

const char *cm_page_state_name(cm_page_state_t s)
{
    if ((unsigned)s >= CM_PAGE_NSTATES) return "?";
    return state_names[s];
}

static void push_free(unsigned order, void *ptr)
{
    cm_free_block_t *b = (cm_free_block_t *)ptr;
    b->next = g_free[order];
    g_free[order] = b;
    g_free_count[order]++;
}

static void *pop_free(unsigned order)
{
    cm_free_block_t *b = g_free[order];
    if (!b) return NULL;
    g_free[order] = b->next;
    g_free_count[order]--;
    return b;
}

static void mark_state(void *ptr, unsigned order, cm_page_state_t st)
{
    if (!g_page_state || !g_heap) return;
    uint64_t off = ((uint8_t *)ptr - g_heap) / CM_PAGE_SIZE;
    uint64_t n = 1ull << order;
    for (uint64_t i = 0; i < n && off + i < g_heap_pages; i++)
        g_page_state[off + i] = (uint8_t)st;
}

void cm_mm_init(const cm_mem_region_t *regions, int nregions)
{
    memset(g_free, 0, sizeof(g_free));
    memset(g_free_count, 0, sizeof(g_free_count));
    g_total_pages = 0;
    g_free_pages = 0;

#ifdef CAPMIX_HOSTED
    /* Simulate a 16 MiB usable arena so buddy/slab/commit are real. */
    g_heap_pages = 16 * 1024 * 1024 / CM_PAGE_SIZE;
    g_sim_heap = (uint8_t *)aligned_alloc(CM_PAGE_SIZE, g_heap_pages * CM_PAGE_SIZE);
    if (!g_sim_heap) cm_panic("hosted heap alloc failed");
    memset(g_sim_heap, 0, g_heap_pages * CM_PAGE_SIZE);
    g_heap = g_sim_heap;
    g_page_state = (uint8_t *)calloc(g_heap_pages, 1);
    CM_UNUSED(regions);
    CM_UNUSED(nregions);
#else
    /* Freestanding: caller must have identity-mapped a contiguous usable span
     * into regions[0]. Later phases walk the full Limine mmap. */
    if (nregions < 1) cm_panic("no memory regions");
    g_heap = (uint8_t *)(uintptr_t)regions[0].base;
    g_heap_pages = regions[0].npages;
    g_page_state = NULL;
#endif

    g_total_pages = g_heap_pages;
    uint64_t remaining = g_heap_pages;
    uint8_t *p = g_heap;
    /* Greedy split into largest aligned buddies. */
    while (remaining) {
        unsigned order = CM_BUDDY_MAX_ORDER;
        while (order > 0 && ((1ull << order) > remaining ||
                             (((uintptr_t)p >> CM_PAGE_SHIFT) & ((1ull << order) - 1))))
            order--;
        push_free(order, p);
        mark_state(p, order, CM_PAGE_FREE);
        uint64_t n = 1ull << order;
        p += n * CM_PAGE_SIZE;
        remaining -= n;
        g_free_pages += n;
    }

    extern void cm_slab_init(void);
    extern void cm_commit_init(uint64_t total_pages);
    cm_slab_init();
    cm_commit_init(g_total_pages);
    kinfo("mm: buddy ready, %llu pages (%llu KiB), max order %d",
          (unsigned long)g_total_pages,
          (unsigned long)(g_total_pages * CM_PAGE_SIZE / 1024),
          CM_BUDDY_MAX_ORDER);
}

void *cm_page_alloc(unsigned order)
{
    if (order > CM_BUDDY_MAX_ORDER) return NULL;
    unsigned o = order;
    while (o <= CM_BUDDY_MAX_ORDER && !g_free[o]) o++;
    if (o > CM_BUDDY_MAX_ORDER) return NULL;
    void *blk = pop_free(o);
    while (o > order) {
        o--;
        uint8_t *buddy = (uint8_t *)blk + ((1ull << o) * CM_PAGE_SIZE);
        push_free(o, buddy);
        mark_state(buddy, o, CM_PAGE_FREE);
    }
    uint64_t n = 1ull << order;
    if (g_free_pages < n) return NULL;
    g_free_pages -= n;
    mark_state(blk, order, CM_PAGE_ACTIVE);
    cm_obs_emit(cm_trace_new(), CM_EV_ALLOC, (uint64_t)(uintptr_t)blk, order, n);
    return blk;
}

void cm_page_free(void *ptr, unsigned order)
{
    if (!ptr || order > CM_BUDDY_MAX_ORDER) return;
    uint64_t n = 1ull << order;
    g_free_pages += n;
    mark_state(ptr, order, CM_PAGE_FREE);
    /* Coalesce: walk up while buddy is sitting on the free list of same order. */
    void *cur = ptr;
    unsigned o = order;
    while (o < CM_BUDDY_MAX_ORDER && g_heap) {
        uintptr_t off = (uintptr_t)((uint8_t *)cur - g_heap);
        uintptr_t size = (1ull << o) * CM_PAGE_SIZE;
        uintptr_t buddy_off = off ^ size;
        void *buddy = g_heap + buddy_off;
        cm_free_block_t **pp = &g_free[o];
        int found = 0;
        while (*pp) {
            if (*pp == (cm_free_block_t *)buddy) {
                *pp = (*pp)->next;
                g_free_count[o]--;
                found = 1;
                break;
            }
            pp = &(*pp)->next;
        }
        if (!found) break;
        cur = (buddy_off < off) ? buddy : cur;
        o++;
    }
    push_free(o, cur);
    cm_obs_emit(cm_trace_new(), CM_EV_FREE, (uint64_t)(uintptr_t)ptr, order, n);
}

void cm_mm_stats(cm_mm_stats_t *out)
{
    extern void cm_commit_stats(uint64_t *committed, uint64_t *limit);
    extern uint64_t cm_slab_pages(void);
    extern cm_pressure_level_t cm_pressure_current(void);
    memset(out, 0, sizeof(*out));
    out->total_pages = g_total_pages;
    out->free_pages  = g_free_pages;
    out->slab_pages  = cm_slab_pages();
    cm_commit_stats(&out->committed_pages, &out->commit_limit);
    out->pressure         = cm_pressure_current();
    out->compressed_pages = cm_compressed_pages();
    out->file_cache_pages = cm_file_cache_pages();
    out->anon_pages       = cm_anon_pages();
    out->swap_slots       = cm_swap_slots();
    out->jetsam_kills     = cm_jetsam_kills();
}

void cm_mm_dump(void)
{
    cm_mm_stats_t s;
    cm_mm_stats(&s);
    kinfo("mm: total=%llu free=%llu commit=%llu/%llu cache=%llu anon=%llu comp=%llu swap=%llu jetsam=%u press=%s",
          (unsigned long)s.total_pages, (unsigned long)s.free_pages,
          (unsigned long)s.committed_pages, (unsigned long)s.commit_limit,
          (unsigned long)s.file_cache_pages, (unsigned long)s.anon_pages,
          (unsigned long)s.compressed_pages, (unsigned long)s.swap_slots,
          s.jetsam_kills, cm_pressure_name(s.pressure));
    for (unsigned o = 0; o <= CM_BUDDY_MAX_ORDER; o++) {
        if (g_free_count[o])
            kinfo("  order %u: %llu free blocks", o, (unsigned long)g_free_count[o]);
    }
}
