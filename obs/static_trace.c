#include "capmix/trace.h"
#include "capmix/arch.h"
#include "capmix/obs.h"
#include "capmix/sched.h"
#include "capmix/cap_internal.h"
#include <string.h>
#define DESC(id,name,category) {CM_TP_##id,name,category,CM_TP_BIT(CM_TP_##id)},
const cm_tp_desc_t cm_tp_catalog[CM_TP_COUNT]={CM_TRACE_POINTS(DESC)};
#undef DESC
uint64_t cm_tp_enabled_mask;
#define SESSIONS 16
typedef struct { uint64_t active,allowed,mask,subject,cpus; } session_t;
static session_t sessions[SESSIONS];
typedef struct { uint64_t stamp; cm_trace_record_t record; } slot_t;
static struct { uint64_t head; slot_t slot[CM_TP_RING_SIZE]; } rings[CM_MAX_CPUS];
static void recompute(void)
{
    uint64_t mask=0;
    for (unsigned i=0;i<SESSIONS;i++) if (sessions[i].active) mask|=sessions[i].mask;
    __atomic_store_n(&cm_tp_enabled_mask,mask,__ATOMIC_RELEASE);
}
static void close_session(void *payload)
{
    uint64_t f=cm_cap_lock(); session_t *s=payload;
    __atomic_store_n(&s->mask,0,__ATOMIC_RELEASE);
    __atomic_store_n(&s->active,0,__ATOMIC_RELEASE); recompute(); cm_cap_unlock(f);
}
void cm_tp_init(void) { memset(rings,0,sizeof(rings)); memset(sessions,0,sizeof(sessions)); cm_tp_enabled_mask=0; }
int cm_trace_open(cm_task_t *reader,uint64_t allowed,uint64_t subject,uint64_t cpus)
{
    if (!reader || !allowed || (allowed&~CM_TP_ALL) || !cpus || (cpus>>CM_MAX_CPUS)) return CM_ERR_INVAL;
    uint64_t f=cm_cap_lock(); session_t *s=NULL;
    for (unsigned i=0;i<SESSIONS;i++) if (!sessions[i].active) { s=&sessions[i]; break; }
    if (s) {
        __atomic_store_n(&s->subject,subject,__ATOMIC_RELAXED);
        __atomic_store_n(&s->cpus,cpus,__ATOMIC_RELAXED);
        s->allowed=allowed; __atomic_store_n(&s->mask,0,__ATOMIC_RELAXED);
        __atomic_store_n(&s->active,1,__ATOMIC_RELEASE);
    }
    cm_cap_unlock(f); if (!s) return CM_ERR_NOSPC;
    cm_object_t *o=cm_obj_mint_owned(CM_OBJ_TRACE,s,CM_RIGHT_TRACE_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE,close_session);
    if (!o) { close_session(s); return CM_ERR_NOMEM; }
    int hid=cm_handle_install(reader,o,o->default_rights); cm_obj_release(o); return hid;
}
cm_error_t cm_trace_configure(cm_task_t *reader,int hid,uint64_t mask)
{
    cm_object_t *o=NULL; cm_error_t e=cm_handle_acquire(reader,hid,CM_OBJ_TRACE,CM_RIGHT_WRITE,&o);
    if (e) return e;
    uint64_t f=cm_cap_lock(); session_t *s=o->payload;
    if (mask&~s->allowed) e=CM_ERR_DENIED;
    else { __atomic_store_n(&s->mask,mask&CM_TP_BUILD_MASK,__ATOMIC_RELEASE); recompute(); }
    cm_cap_unlock(f); cm_obj_release(o); return e;
}
uint64_t cm_trace_head(unsigned cpu) { return cpu<CM_MAX_CPUS ? __atomic_load_n(&rings[cpu].head,__ATOMIC_ACQUIRE) : 0; }
void cm_tp_emit(unsigned tp,cm_trace_t tr,uint64_t a,uint64_t b,uint64_t c)
{
    if (tp>=CM_TP_COUNT) return;
    uint64_t flags=cm_irq_save(); unsigned cpu=(unsigned)cm_cpu_id();
    if (cpu>=CM_MAX_CPUS) { cm_irq_restore(flags); return; }
    cm_thread_t *current=cm_sched_current();
    uint64_t task=current && current->task ? current->task->id : 0;
    bool capture=false;
    for (unsigned i=0;i<SESSIONS;i++) {
        session_t *s=&sessions[i];
        if (__atomic_load_n(&s->active,__ATOMIC_ACQUIRE) &&
            (__atomic_load_n(&s->mask,__ATOMIC_ACQUIRE)&CM_TP_BIT(tp)) &&
            (__atomic_load_n(&s->cpus,__ATOMIC_RELAXED)&(1ull<<cpu)) &&
            (!__atomic_load_n(&s->subject,__ATOMIC_RELAXED) || __atomic_load_n(&s->subject,__ATOMIC_RELAXED)==task)) capture=true;
    }
    if (!capture) { cm_irq_restore(flags); return; }
    if (!tr) tr=current && current->trace_context ? current->trace_context : cm_trace_new();
    uint64_t seq=__atomic_load_n(&rings[cpu].head,__ATOMIC_RELAXED)+1;
    slot_t *slot=&rings[cpu].slot[(seq-1)%CM_TP_RING_SIZE];
    cm_trace_record_t r={cm_rdtsc(),cpu,task,current ? current->id : 0,tr,
        (tr&0xffff)>1 ? tr-1 : 0,seq,tp,a,b,c};
    __atomic_store_n(&slot->stamp,seq*2-1,__ATOMIC_SEQ_CST);
    uint64_t *src=(uint64_t *)&r,*dst=(uint64_t *)&slot->record;
    for (unsigned i=0;i<sizeof(r)/8;i++) __atomic_store_n(dst+i,src[i],__ATOMIC_RELAXED);
    __atomic_store_n(&slot->stamp,seq*2,__ATOMIC_RELEASE);
    __atomic_store_n(&rings[cpu].head,seq,__ATOMIC_RELEASE);
    cm_irq_restore(flags);
}
cm_error_t cm_trace_read(cm_task_t *reader,int hid,unsigned cpu,cm_trace_read_t *out)
{
    if (!out || cpu>=CM_MAX_CPUS) return CM_ERR_INVAL;
    cm_object_t *o=NULL; cm_error_t e=cm_handle_acquire(reader,hid,CM_OBJ_TRACE,CM_RIGHT_TRACE_READ,&o);
    if (e) return e;
    session_t *s=o->payload;
    if (!(__atomic_load_n(&s->cpus,__ATOMIC_ACQUIRE)&(1ull<<cpu))) { cm_obj_release(o); return CM_ERR_DENIED; }
    uint64_t head=cm_trace_head(cpu);
    if (out->cursor>head) { cm_obj_release(o); return CM_ERR_INVAL; }
    uint64_t earliest=head>CM_TP_RING_SIZE ? head-CM_TP_RING_SIZE : 0;
    if (out->cursor<earliest) { out->lost+=earliest-out->cursor; out->cursor=earliest; }
    e=CM_ERR_AGAIN;
    while (out->cursor<head) {
        uint64_t seq=out->cursor+1; slot_t *slot=&rings[cpu].slot[(seq-1)%CM_TP_RING_SIZE];
        uint64_t stamp=__atomic_load_n(&slot->stamp,__ATOMIC_ACQUIRE);
        if (stamp!=seq*2) break;
        cm_trace_record_t r; uint64_t *dst=(uint64_t *)&r,*src=(uint64_t *)&slot->record;
        for (unsigned i=0;i<sizeof(r)/8;i++) dst[i]=__atomic_load_n(src+i,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&slot->stamp,__ATOMIC_ACQUIRE)!=stamp) break;
        out->cursor=seq;
        if (r.tp>=CM_TP_COUNT || !(s->allowed&CM_TP_BIT(r.tp)) || (s->subject && r.task!=s->subject)) continue;
        out->event=r; e=CM_OK; break;
    }
    cm_obj_release(o); return e;
}

void cm_trace_process_exit(cm_task_t *task)
{
    if (!task) return;
    for (int hid=1;hid<CM_HANDLES_PER_TASK;hid++) {
        uint64_t flags=cm_cap_lock(); cm_handle_t *h=cm_handle_lookup(task,hid);
        bool trace=h && h->obj->type==CM_OBJ_TRACE; cm_cap_unlock(flags);
        if (trace) cm_handle_drop(task,hid);
    }
}
