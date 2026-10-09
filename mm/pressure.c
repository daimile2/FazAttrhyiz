#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/config.h"
#include "capmix/klog.h"
#include "capmix/obs.h"
#include "capmix/proc.h"

#include <string.h>

/* Four-level pressure pipeline (macOS) driven by Linux-style watermarks.
 *
 *   free >= high          nothing to do
 *   free <  low           an allocation reclaims a 32-page cluster first
 *                         (direct reclaim, bounded cost per allocation)
 *     L1  drop clean file-cache pages        (free: disk has the source)
 *     L2  compress cold anon pages           (WKdm, ~10-100x faster than I/O)
 *     L3  write compressor segments to swap  (compressed: less I/O, less wear)
 *   free <= min           full balance to high; if L1..L3 cannot get
 *                         there: L4 jetsam by band, Idle first,
 *                         foreground never; Linux OOM killer only if no band
 *                         candidate is left.
 *
 * Commit accounting (commit.c) stays in front of all of this: anon memory is
 * charged when allocated, so the pipeline is the way pages get FOUND for
 * commitments already made, not a gamble that they will appear. */

static cm_pressure_level_t g_level = CM_PRESS_IDLE;
static cm_pressure_stats_t g_ps;
static cm_watermarks_t     g_wm;

extern uint64_t g_anon_l2_pages, g_anon_l2_incomp, g_anon_l3_segs, g_anon_l3_pages;
extern uint64_t g_anon_swapins, g_anon_decomp, g_anon_lost, g_anon_compact;

/* Recursive: reclaim -> jetsam -> cm_anon_free_owner re-enters. */
static volatile int g_mm_owner;
static int          g_mm_depth;
static uint64_t     g_mm_flags;

void cm_mm_lock(void)
{
    uint64_t f = cm_irq_save();
    int me = cm_cpu_id() + 1;
    if (g_mm_owner == me) { g_mm_depth++; cm_irq_restore(f); return; }
    while (!__sync_bool_compare_and_swap(&g_mm_owner, 0, me))
        __asm__ volatile("pause" ::: "memory");
    g_mm_depth = 1;
    g_mm_flags = f;
}

void cm_mm_unlock(void)
{
    if (--g_mm_depth > 0) return;
    uint64_t f = g_mm_flags;
    __sync_lock_release(&g_mm_owner);
    cm_irq_restore(f);
}

void cm_pressure_init(void)
{
    uint64_t t = cm_mm_total_pages();
    g_wm.min  = t / 64 < 16 ? 16 : t / 64;
    g_wm.low  = g_wm.min * 2;
    g_wm.high = g_wm.min * 3;
    memset(&g_ps, 0, sizeof(g_ps));
    g_level = CM_PRESS_IDLE;
    kinfo("mm: watermarks min=%llu low=%llu high=%llu pages",
          (unsigned long)g_wm.min, (unsigned long)g_wm.low, (unsigned long)g_wm.high);
}

void cm_mm_watermarks(cm_watermarks_t *out) { if (out) *out = g_wm; }
cm_pressure_level_t cm_pressure_current(void) { return g_level; }
uint32_t cm_jetsam_kills(void) { return g_ps.jetsam_kills; }
uint32_t cm_oom_kills(void)    { return g_ps.oom_kills; }

void cm_pressure_stats(cm_pressure_stats_t *out)
{
    if (!out) return;
    *out = g_ps;
    out->l2_compressed     = g_anon_l2_pages;
    out->l2_incompressible = g_anon_l2_incomp;
    out->l3_segments_out   = g_anon_l3_segs;
    out->l3_pages_out      = g_anon_l3_pages;
    out->swapins           = g_anon_swapins;
    out->decompressions    = g_anon_decomp;
    out->lost_pages        = g_anon_lost;
    out->l3_compactions    = g_anon_compact;
}

void cm_pressure_reset_max(void) { g_ps.max_alloc_work = 0; }

/* Work units = pages dropped + compression attempts + segments written or
 * compacted. Deterministic, unlike wall-clock latency inside a VM whose
 * vCPU the host may deschedule at any moment. */
static uint64_t work_units(void)
{
    return g_ps.l1_discarded + g_anon_l2_pages + g_anon_l2_incomp +
           g_anon_l3_segs + g_anon_compact;
}

const char *cm_pressure_name(cm_pressure_level_t l)
{
    switch (l) {
    case CM_PRESS_IDLE:        return "idle";
    case CM_PRESS_L1_DISCARD:  return "L1-discard";
    case CM_PRESS_L2_COMPRESS: return "L2-compress";
    case CM_PRESS_L3_COMPSWAP: return "L3-compswap";
    case CM_PRESS_L4_JETSAM:   return "L4-jetsam";
    default:                   return "?";
    }
}

static void reached(cm_pressure_level_t l)
{
    if (l > g_level) g_level = l;
    if (l > g_ps.deepest) g_ps.deepest = l;
}

cm_pressure_level_t cm_pressure_tick(void)
{
    if (cm_mm_free_pages() >= g_wm.high) g_level = CM_PRESS_IDLE;
    if (g_level != CM_PRESS_IDLE)
        cm_obs_emit(cm_trace_new(), CM_EV_PRESSURE, (uint64_t)g_level,
                    cm_mm_free_pages(), 0);
    return g_level;
}

uint64_t cm_pressure_reclaim(uint64_t want_free)
{
    cm_mm_lock();
    uint64_t start = cm_mm_free_pages();
    uint64_t target = start + want_free;
#define FREE_NOW() cm_mm_free_pages()

    uint64_t n = cm_cache_discard_clean(want_free);
    if (n) {
        g_ps.l1_discarded += n;
        reached(CM_PRESS_L1_DISCARD);
        cm_obs_emit(cm_trace_new(), CM_EV_PRESSURE, CM_PRESS_L1_DISCARD, n, 0);
    }
    /* L2 and L3 alternate: swapping segments out empties the pool, which
     * lets L2 compress more without the pool itself eating the gain. */
    for (int round = 0; round < 16 && FREE_NOW() < target; round++) {
        uint64_t c = cm_anon_compress(target - FREE_NOW());
        if (c) {
            reached(CM_PRESS_L2_COMPRESS);
            cm_obs_emit(cm_trace_new(), CM_EV_PRESSURE, CM_PRESS_L2_COMPRESS, c, 0);
        }
        if (FREE_NOW() >= target) break;
        uint64_t s = cm_comp_swapout(target - FREE_NOW());
        if (s) {
            reached(CM_PRESS_L3_COMPSWAP);
            cm_obs_emit(cm_trace_new(), CM_EV_PRESSURE, CM_PRESS_L3_COMPSWAP, s, 0);
        }
        if (!c && !s) break;
    }
    uint64_t end = FREE_NOW();
#undef FREE_NOW
    cm_mm_unlock();
    return end > start ? end - start : 0;
}

/* L4. Returns 1 if a process was killed and its memory released. */
static int kill_one(void)
{
    int oom = 0;
    cm_id_t pid = cm_proc_jetsam_pick(&oom);
    if (!pid) return 0;
    uint64_t freed = cm_anon_free_owner(pid);
    if (oom) g_ps.oom_kills++; else g_ps.jetsam_kills++;
    reached(CM_PRESS_L4_JETSAM);
    kwarn("pressure L4: %s killed pid=%llu, released %llu anon pages",
          oom ? "OOM killer" : "jetsam", (unsigned long)pid, (unsigned long)freed);
    cm_obs_emit(cm_trace_new(), CM_EV_OOM, pid, freed, (uint64_t)oom);
    return 1;
}

int cm_mm_balance(int need_pages)
{
    cm_mm_lock();
    uint64_t need = need_pages > 0 ? (uint64_t)need_pages : 0;
    uint64_t f = cm_mm_free_pages();
    if (f < g_wm.high + need) cm_pressure_reclaim(g_wm.high + need - f);
    while (cm_mm_free_pages() <= g_wm.min + need) {
        if (!kill_one()) break;
        f = cm_mm_free_pages();
        if (f < g_wm.high + need) cm_pressure_reclaim(g_wm.high + need - f);
    }
    int ok = cm_mm_free_pages() > g_wm.min;
    cm_mm_unlock();
    return ok;
}

/* Direct reclaim works in small clusters (Linux SWAP_CLUSTER_MAX): an
 * allocation that finds free < low pays for 32 pages of L1..L3, not for a
 * whole refill to `high`. Refilling to high in one go made a single
 * unlucky allocation absorb ~400 compressions — 4 ms on bare metal.
 * The cost is spread over the allocations that follow instead. */
#define RECLAIM_CLUSTER 32

void *cm_page_alloc_user(void)
{
    uint64_t w0 = work_units();
    if (cm_mm_free_pages() < g_wm.low) {
        g_ps.direct_reclaims++;
        cm_pressure_reclaim(RECLAIM_CLUSTER);
    }
    /* At min, L1..L3 alone did not keep up: full balance, L4 included. */
    if (cm_mm_free_pages() <= g_wm.min) cm_mm_balance(1);
    uint64_t w = work_units() - w0;
    if (w > g_ps.max_alloc_work) g_ps.max_alloc_work = w;
    /* The min reserve belongs to the reclaim path (pool pages, swap-in,
     * page tables of the dying). A user allocation never takes it. */
    if (cm_mm_free_pages() <= g_wm.min) {
        g_ps.alloc_failures++;
        return NULL;
    }
    void *p = cm_page_alloc(0);
    if (!p) g_ps.alloc_failures++;
    return p;
}
