#include "capmix/obs.h"
#include "capmix/config.h"
#include "capmix/klog.h"
#include "capmix/arch.h"
#include "capmix/sched.h"

static cm_event_t g_ring[CM_TRACE_RING_SIZE];
static uint32_t   g_head;
static uint64_t   g_count;
static uint64_t   g_trace_seq;
/* Default OFF.
 * Observation is a capability, not a global switch: one emit costs
 * ~447 cycles (ticks + current-thread lookup), and a single IPC
 * round trip emits ~5 times -> 2198 cycles of tax, more than the
 * entire IPC cost itself. Pay it only while a session is open and
 * authorized. cm_obs_open_session()/cm_obs_authorize() turn it on.
 */
static int        g_obs_on = 0;

extern void cm_obs_etw_init(void);

void cm_obs_init(void)
{
    g_head = 0;
    g_count = 0;
    g_trace_seq = 1;
    g_obs_on = 0;
    cm_obs_etw_init();
}

void cm_obs_enable(int on) { g_obs_on = on ? 1 : 0; }
int  cm_obs_enabled(void) { return g_obs_on; }

cm_trace_t cm_trace_new(void)
{
    if (!g_obs_on) return 0;
    return ++g_trace_seq;
}

cm_trace_t cm_trace_child(cm_trace_t parent)
{
    /* Low 16 bits = hop count so a causal chain is greppable. */
    cm_trace_t hop = (parent & 0xFFFFu) + 1;
    return (cm_trace_new() << 16) ^ (parent & ~0xFFFFull) ^ hop;
}

void cm_obs_emit(cm_trace_t tr, cm_event_kind_t kind,
                 uint64_t a0, uint64_t a1, uint64_t a2)
{
    if (!g_obs_on) return;
    uint32_t i = g_head++ % CM_TRACE_RING_SIZE;
    cm_thread_t *cur = cm_sched_current();
    g_ring[i].ts    = cm_ticks_now();
    g_ring[i].trace = tr;
    g_ring[i].kind  = kind;
    g_ring[i].cpu   = 0;
    g_ring[i].tid   = cur ? (uint32_t)cur->id : 0;
    g_ring[i].a0    = a0;
    g_ring[i].a1    = a1;
    g_ring[i].a2    = a2;
    g_count++;
}

uint64_t cm_obs_count(void) { return g_count; }

static const char *kind_name(cm_event_kind_t k)
{
    switch (k) {
    case CM_EV_BOOT:         return "boot";
    case CM_EV_ALLOC:        return "alloc";
    case CM_EV_FREE:         return "free";
    case CM_EV_IPC_SEND:     return "ipc.send";
    case CM_EV_IPC_RECV:     return "ipc.recv";
    case CM_EV_IPC_HANDOFF:  return "ipc.handoff";
    case CM_EV_SCHED_PICK:   return "sched.pick";
    case CM_EV_SCHED_SWITCH: return "sched.switch";
    case CM_EV_QOS_BOOST:    return "qos.boost";
    case CM_EV_TURNSTILE:    return "turnstile";
    case CM_EV_PRESSURE:     return "pressure";
    case CM_EV_OOM:          return "oom";
    case CM_EV_CAP_MINT:     return "cap.mint";
    case CM_EV_CAP_DROP:     return "cap.drop";
    case CM_EV_SYS:          return "sys";
    case CM_EV_NET:          return "net";
    case CM_EV_FS:           return "fs";
    default:                 return "?";
    }
}

void cm_obs_dump(int max_events)
{
    uint64_t n = g_count < CM_TRACE_RING_SIZE ? g_count : CM_TRACE_RING_SIZE;
    if (max_events > 0 && (uint64_t)max_events < n) n = (uint64_t)max_events;
    kinfo("obs: %llu events (showing last %llu)", (unsigned long)g_count, (unsigned long)n);
    uint32_t start = (g_head - (uint32_t)n) % CM_TRACE_RING_SIZE;
    for (uint64_t i = 0; i < n; i++) {
        cm_event_t *e = &g_ring[(start + (uint32_t)i) % CM_TRACE_RING_SIZE];
        kinfo("  tr=%llx kind=%s tid=%u a0=%llx a1=%llx a2=%llx",
              (unsigned long)e->trace, kind_name(e->kind), e->tid,
              (unsigned long)e->a0, (unsigned long)e->a1, (unsigned long)e->a2);
    }
}
