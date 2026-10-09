#include "capmix/mm.h"
#include "capmix/io.h"
#include "capmix/klog.h"

#include <string.h>

/* Unified page cache for the COW volume, keyed by fs block number.
 *
 * Why the block number is a safe key: a COW block is never rewritten while
 * anything references it, so "block b" means the same bytes until the FS
 * frees and re-allocates b — and cowfs invalidates on allocation. The one
 * in-place writer (nocow) invalidates/refills explicitly.
 *
 * Every cached page is clean: cowfs writes through to the device before it
 * fills the cache. That makes the whole cache the L1 reclaim source —
 * dropping a page costs nothing, the disk still has the source (macOS
 * "discard file-backed pages first"). A clock hand with a referenced bit
 * keeps recently-read blocks around longest. */

#define PC_BLKS (CM_BLK_FS_SECT / (CM_PAGE_SIZE / CM_BLK_SECTOR))

static void    *g_pc[PC_BLKS];
static uint8_t  g_pc_ref[PC_BLKS];
static uint32_t g_hand;
static uint64_t g_pages, g_hits, g_misses;

uint64_t cm_file_cache_pages(void) { return g_pages; }
uint64_t cm_pcache_hits(void)      { return g_hits; }
uint64_t cm_pcache_misses(void)    { return g_misses; }

void cm_pcache_init(void)
{
    memset(g_pc, 0, sizeof(g_pc));
    memset(g_pc_ref, 0, sizeof(g_pc_ref));
    g_hand = 0;
    g_pages = g_hits = g_misses = 0;
}

int cm_pcache_read(uint32_t blk, void *dst)
{
    if (blk >= PC_BLKS) return 0;
    cm_mm_lock();
    int hit = g_pc[blk] != NULL;
    if (hit) {
        memcpy(dst, g_pc[blk], CM_PAGE_SIZE);
        g_pc_ref[blk] = 1;
        g_hits++;
    } else {
        g_misses++;
    }
    cm_mm_unlock();
    return hit;
}

void cm_pcache_fill(uint32_t blk, const void *src)
{
    if (blk >= PC_BLKS) return;
    cm_mm_lock();
    if (g_pc[blk]) {
        memcpy(g_pc[blk], src, CM_PAGE_SIZE);
        cm_mm_unlock();
        return;
    }
    /* Opportunistic: the cache never pushes anyone into reclaim. Below the
     * low watermark a read simply is not cached. */
    cm_watermarks_t wm;
    cm_mm_watermarks(&wm);
    void *pg = cm_mm_free_pages() > wm.low ? cm_page_alloc(0) : NULL;
    if (pg) {
        memcpy(pg, src, CM_PAGE_SIZE);
        g_pc[blk] = pg;
        g_pc_ref[blk] = 1;
        g_pages++;
    }
    cm_mm_unlock();
}

static void drop(uint32_t b)
{
    cm_page_free(g_pc[b], 0);
    g_pc[b] = NULL;
    g_pc_ref[b] = 0;
    g_pages--;
}

void cm_pcache_invalidate(uint32_t blk)
{
    if (blk >= PC_BLKS) return;
    cm_mm_lock();
    if (g_pc[blk]) drop(blk);
    cm_mm_unlock();
}

void cm_pcache_invalidate_all(void)
{
    cm_mm_lock();
    for (uint32_t b = 0; b < PC_BLKS; b++) if (g_pc[b]) drop(b);
    cm_mm_unlock();
}

uint64_t cm_cache_discard_clean(uint64_t want)
{
    uint64_t n = 0;
    cm_mm_lock();
    /* Two sweeps: the first clears referenced bits, the second can then
     * take anything, so a fully-hot cache still yields under pressure. */
    for (uint32_t step = 0; step < 2 * PC_BLKS && n < want && g_pages; step++) {
        uint32_t b = g_hand;
        g_hand = (g_hand + 1) % PC_BLKS;
        if (!g_pc[b]) continue;
        if (g_pc_ref[b]) { g_pc_ref[b] = 0; continue; }
        drop(b);
        n++;
    }
    cm_mm_unlock();
    return n;
}
