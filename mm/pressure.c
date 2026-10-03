#include "capmix/mm.h"
#include "capmix/config.h"
#include "capmix/klog.h"
#include "capmix/obs.h"
#include "capmix/proc.h"

static cm_pressure_level_t g_level = CM_PRESS_IDLE;
static uint32_t g_jetsam_kills;

cm_pressure_level_t cm_pressure_current(void) { return g_level; }
uint32_t cm_jetsam_kills(void) { return g_jetsam_kills; }
void cm_pressure_note_jetsam(void) { g_jetsam_kills++; }

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

static cm_pressure_level_t watermark(const cm_mm_stats_t *s)
{
    if (s->total_pages == 0) return CM_PRESS_IDLE;
    uint64_t used = s->total_pages - s->free_pages;
    uint64_t pct = used * 100 / s->total_pages;
    if (pct < 50) return CM_PRESS_IDLE;
    if (pct < 65) return CM_PRESS_L1_DISCARD;
    if (pct < 80) return CM_PRESS_L2_COMPRESS;
    if (pct < 92) return CM_PRESS_L3_COMPSWAP;
    return CM_PRESS_L4_JETSAM;
}

cm_pressure_level_t cm_pressure_tick(void)
{
    cm_mm_stats_t s;
    cm_mm_stats(&s);
    g_level = watermark(&s);
    if (g_level != CM_PRESS_IDLE)
        cm_obs_emit(cm_trace_new(), CM_EV_PRESSURE, (uint64_t)g_level,
                    s.free_pages, s.committed_pages);
    return g_level;
}

uint64_t cm_pressure_reclaim(uint64_t want_free)
{
    /* Walk L1→L4 until free pages meet the request or nothing left.
     * Never panics; jetsam is last and still leaves the kernel up. */
    uint64_t got = 0;
    cm_mm_stats_t s;
    cm_mm_stats(&s);
    uint64_t target = s.free_pages + want_free;

    uint64_t n = cm_cache_discard_clean(want_free);
    if (n) {
        kinfo("pressure L1: discarded %llu clean file pages", (unsigned long)n);
        got += n;
        cm_obs_emit(cm_trace_new(), CM_EV_PRESSURE, CM_PRESS_L1_DISCARD, n, 0);
    }
    cm_mm_stats(&s);
    if (s.free_pages >= target) { g_level = watermark(&s); return got; }

    n = cm_anon_compress(want_free - got);
    if (n) {
        kinfo("pressure L2: compressed %llu anon pages", (unsigned long)n);
        got += n;
        cm_obs_emit(cm_trace_new(), CM_EV_PRESSURE, CM_PRESS_L2_COMPRESS, n, 0);
    }
    cm_mm_stats(&s);
    if (s.free_pages >= target) { g_level = watermark(&s); return got; }

    n = cm_comp_swapout(want_free);
    if (n) {
        kinfo("pressure L3: swapped %llu compressed pages", (unsigned long)n);
        cm_obs_emit(cm_trace_new(), CM_EV_PRESSURE, CM_PRESS_L3_COMPSWAP, n, 0);
    }

    cm_mm_stats(&s);
    if (s.free_pages < target) {
        int killed = cm_proc_jetsam();
        if (killed) {
            kwarn("pressure L4: jetsam (idle→bg→utility→default→foreground)");
            cm_obs_emit(cm_trace_new(), CM_EV_OOM, 1, 0, 0);
            got += 1;
        }
    }
    cm_mm_stats(&s);
    g_level = watermark(&s);
    return got;
}
