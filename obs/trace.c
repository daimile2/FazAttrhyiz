#include "capmix/obs.h"
#include "capmix/trace.h"
#include "capmix/config.h"
#include "capmix/klog.h"
#include "capmix/arch.h"
#include "capmix/sched.h"

static struct { uint64_t stamp; cm_event_t event; } g_ring[CM_MAX_CPUS][CM_TRACE_RING_SIZE];
static uint64_t g_head[CM_MAX_CPUS];
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
    for (unsigned cpu=0;cpu<CM_MAX_CPUS;cpu++) g_head[cpu]=0;
    g_trace_seq = 1;
    g_obs_on = 0;
    cm_obs_etw_init();
    cm_tp_init();
}

void cm_obs_enable(int on) { __atomic_store_n(&g_obs_on,on ? 1 : 0,__ATOMIC_RELEASE); }
int  cm_obs_enabled(void) { return __atomic_load_n(&g_obs_on,__ATOMIC_RELAXED) || __atomic_load_n(&cm_tp_enabled_mask,__ATOMIC_RELAXED); }

cm_trace_t cm_trace_new(void)
{
    if (!cm_obs_enabled()) return 0;
    return (__atomic_add_fetch(&g_trace_seq,1,__ATOMIC_RELAXED)<<16)|1;
}

cm_trace_t cm_trace_child(cm_trace_t parent)
{
    if (!parent) return cm_trace_new();
    if ((parent&0xffff)==0xffff) return cm_trace_new();
    return parent+1; /* root high bits stay stable; low bits count chain hops */
}

void cm_obs_emit(cm_trace_t tr, cm_event_kind_t kind,
                 uint64_t a0, uint64_t a1, uint64_t a2)
{
    if (!__atomic_load_n(&g_obs_on,__ATOMIC_RELAXED)) return;
    uint64_t flags=cm_irq_save(); unsigned cpu=(unsigned)cm_cpu_id();
    if (cpu>=CM_MAX_CPUS) { cm_irq_restore(flags); return; }
    uint64_t seq=__atomic_load_n(&g_head[cpu],__ATOMIC_RELAXED)+1;
    unsigned i=(unsigned)((seq-1)%CM_TRACE_RING_SIZE);
    cm_thread_t *cur=cm_sched_current();
    cm_event_t *e=&g_ring[cpu][i].event;
    __atomic_store_n(&g_ring[cpu][i].stamp,seq*2-1,__ATOMIC_SEQ_CST);
#define STORE(field,value) __atomic_store_n(&e->field,(value),__ATOMIC_RELAXED)
    STORE(ts,cm_rdtsc()); STORE(trace,tr); STORE(kind,kind); STORE(cpu,cpu);
    STORE(tid,cur ? (uint32_t)cur->id : 0); STORE(a0,a0); STORE(a1,a1); STORE(a2,a2);
#undef STORE
    __atomic_store_n(&g_ring[cpu][i].stamp,seq*2,__ATOMIC_RELEASE);
    __atomic_store_n(&g_head[cpu],seq,__ATOMIC_RELEASE);
    cm_irq_restore(flags);
}
uint64_t cm_obs_count(void)
{
    uint64_t count=0;
    for (unsigned cpu=0;cpu<CM_MAX_CPUS;cpu++) count+=__atomic_load_n(&g_head[cpu],__ATOMIC_ACQUIRE);
    return count;
}

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
    kinfo("obs: %llu events (per-CPU legacy snapshots)",(unsigned long)cm_obs_count());
    for (unsigned cpu=0;cpu<CM_MAX_CPUS;cpu++) {
        uint64_t head=__atomic_load_n(&g_head[cpu],__ATOMIC_ACQUIRE);
        uint64_t n=head<CM_TRACE_RING_SIZE ? head : CM_TRACE_RING_SIZE;
        if (max_events>0 && n>(unsigned)max_events) n=(unsigned)max_events;
        for (uint64_t seq=head-n+1;seq<=head && n;seq++) {
            unsigned i=(unsigned)((seq-1)%CM_TRACE_RING_SIZE);
            uint64_t stamp=__atomic_load_n(&g_ring[cpu][i].stamp,__ATOMIC_ACQUIRE);
            if (stamp!=seq*2) continue;
            cm_event_t snapshot,*e=&snapshot,*source=&g_ring[cpu][i].event;
#define LOAD(field) e->field=__atomic_load_n(&source->field,__ATOMIC_RELAXED)
            LOAD(ts); LOAD(trace); LOAD(kind); LOAD(cpu); LOAD(tid); LOAD(a0); LOAD(a1); LOAD(a2);
#undef LOAD
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&g_ring[cpu][i].stamp,__ATOMIC_ACQUIRE)!=stamp) continue;
            kinfo("  cpu=%u tr=%llx kind=%s tid=%u a0=%llx a1=%llx a2=%llx",e->cpu,
                  (unsigned long)e->trace,kind_name(e->kind),e->tid,
                  (unsigned long)e->a0,(unsigned long)e->a1,(unsigned long)e->a2);
        }
    }
}
