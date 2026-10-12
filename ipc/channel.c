#include "capmix/trace.h"
#include "capmix/ipc.h"
#include "capmix/cap_internal.h"
#include "capmix/sched.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/arch.h"

#include <string.h>

/* L4-style synchronous rendezvous.
 * If a peer is already blocked in recv, we hand the message over and
 * conceptually do a direct context switch (no run-queue hop).
 * Small payloads live in the message struct ("registers").
 * Large payloads use queued object references (e.g. VMO caps), never buffer copies.
 */

#define CM_CHAN_DEPTH 8

typedef struct {
    cm_ipc_msg_t msg;
    cm_handle_t caps[CM_IPC_MAX_HANDLES]; /* each slot owns transferred refs */
} cm_queued_msg_t;

typedef struct {
    cm_queued_msg_t slots[CM_CHAN_DEPTH];
    int          head, tail, count;
    cm_thread_t *recv_waiter;
    cm_thread_t *send_waiter;
    cm_task_t   *recv_task;
    int          recv_hid_hint;
} cm_endp_t;

struct cm_channel {
    cm_id_t   id;
    cm_endp_t a;
    cm_endp_t b;
    int       live;
    unsigned  endpoints;
    int       closed[2];
};

static cm_channel_t g_chans[CM_MAX_CHANNELS];
static cm_id_t      g_chan_id = 1;
static uint64_t     g_rtt_cycles_acc;
static uint64_t     g_rtt_samples;

static cm_channel_t *alloc_chan(void)
{
    for (int i = 0; i < CM_MAX_CHANNELS; i++)
        if (!g_chans[i].live) {
            memset(&g_chans[i], 0, sizeof(g_chans[i]));
            g_chans[i].live = 1;
            g_chans[i].id = g_chan_id++;
            return &g_chans[i];
        }
    return NULL;
}

static cm_endp_t *local_end(cm_channel_t *c, int side)  { return side ? &c->b : &c->a; }
static cm_endp_t *remote_end(cm_channel_t *c, int side) { return side ? &c->a : &c->b; }

static cm_error_t endpoint_locked(cm_task_t *t, int hid, cm_rights_t right,
                                   cm_channel_t **out, int *side, cm_object_t **obj)
{
    cm_handle_t *h = cm_handle_lookup(t, hid);
    if (!h || h->obj->type != CM_OBJ_ENDPOINT) return CM_ERR_INVAL;
    if (!(h->rights & right)) return CM_ERR_DENIED;
    uintptr_t tag = (uintptr_t)h->obj->payload;
    *side = (int)(tag & 1);
    *out = (cm_channel_t *)(tag & ~(uintptr_t)1);
    if (!(*out)->live || (*out)->closed[*side]) return CM_ERR_DEAD;
    *obj = h->obj;
    return CM_OK;
}

static void endpoint_destroy(void *payload)
{
    uintptr_t tag = (uintptr_t)payload;
    cm_channel_t *c = (cm_channel_t *)(tag & ~(uintptr_t)1);
    int side = (int)(tag & 1);
    cm_object_t *drops[CM_CHAN_DEPTH * CM_IPC_MAX_HANDLES];
    unsigned ndrops = 0;
    uint64_t flags = cm_cap_lock();
    cm_endp_t *e = local_end(c, side);
    while (e->count) {
        cm_queued_msg_t *q = &e->slots[e->head];
        for (unsigned i = 0; i < q->msg.nhandles; i++) drops[ndrops++] = q->caps[i].obj;
        memset(q, 0, sizeof(*q));
        e->head = (e->head + 1) % CM_CHAN_DEPTH;
        e->count--;
    }
    c->closed[side] = 1;
    cm_thread_t *w = remote_end(c, side)->recv_waiter;
    remote_end(c, side)->recv_waiter = NULL;
    if (--c->endpoints == 0) c->live = 0;
    cm_cap_unlock(flags);
    for (unsigned i = 0; i < ndrops; i++) cm_obj_release(drops[i]);
    if (w) cm_thread_wake(w);
}

cm_error_t cm_channel_create(cm_task_t *owner, int *out_a, int *out_b)
{
    if (!owner || !out_a || !out_b || out_a == out_b) return CM_ERR_INVAL;
    uint64_t flags = cm_cap_lock();
    cm_channel_t *c = alloc_chan();
    if (c) c->endpoints = 2;
    cm_cap_unlock(flags);
    if (!c) return CM_ERR_NOSPC;
    cm_rights_t rights = CM_RIGHT_READ | CM_RIGHT_WRITE | CM_RIGHT_TRANSFER | CM_RIGHT_DUPLICATE;
    cm_object_t *ea = cm_obj_mint_owned(CM_OBJ_ENDPOINT, (void *)(uintptr_t)c, rights, endpoint_destroy);
    cm_object_t *eb = cm_obj_mint_owned(CM_OBJ_ENDPOINT, (void *)((uintptr_t)c | 1), rights, endpoint_destroy);
    if (!ea || !eb) {
        if (ea) cm_obj_release(ea); else endpoint_destroy((void *)(uintptr_t)c);
        if (eb) cm_obj_release(eb); else endpoint_destroy((void *)((uintptr_t)c | 1));
        return CM_ERR_NOSPC;
    }
    flags = cm_cap_lock();
    int ha = cm_handle_install_locked(owner, ea, rights);
    int hb = ha > 0 ? cm_handle_install_locked(owner, eb, rights) : CM_ERR_NOSPC;
    if (hb < 0 && ha > 0) memset(&owner->handles[ha], 0, sizeof(cm_handle_t));
    cm_cap_unlock(flags);
    if (hb < 0 && ha > 0) cm_obj_release(ea); /* rollback installed handle */
    cm_obj_release(ea); cm_obj_release(eb); /* drop creator refs */
    if (ha < 0 || hb < 0) return CM_ERR_NOSPC;
    *out_a = ha; *out_b = hb;
    return CM_OK;
}

static cm_error_t send_message(cm_task_t *sender, int hid, const cm_ipc_msg_t *msg, int direct)
{
    if (!msg || msg->nhandles > CM_IPC_MAX_HANDLES || msg->len > CM_IPC_SMALL_BYTES)
        return CM_ERR_INVAL;
    cm_queued_msg_t q;
    memset(&q, 0, sizeof(q));
    q.msg = *msg;
    if (!q.msg.trace && cm_obs_enabled()) q.msg.trace = cm_trace_new();
    uint64_t flags = cm_cap_lock();
    cm_channel_t *c;
    cm_object_t *endpoint;
    int side;
    cm_error_t err = endpoint_locked(sender, hid, CM_RIGHT_WRITE, &c, &side, &endpoint);
    if (err) { cm_cap_unlock(flags); return err; }
    cm_endp_t *remote = remote_end(c, side);
    if (c->closed[side ^ 1]) err = CM_ERR_DEAD;
    else if (remote->count == CM_CHAN_DEPTH) err = CM_ERR_AGAIN;
    for (unsigned i = 0; !err && i < msg->nhandles; i++) {
        cm_handle_t *h = cm_handle_lookup(sender, msg->handles[i]);
        if (!h) { err = CM_ERR_INVAL; break; }
        if (!(h->rights & CM_RIGHT_TRANSFER)) { err = CM_ERR_DENIED; break; }
        for (unsigned j = 0; j < i; j++)
            if (msg->handles[i] == msg->handles[j]) err = CM_ERR_INVAL;
        /* Avoid a channel retaining its own endpoints in an undeliverable cycle. */
        if (h->obj->type == CM_OBJ_ENDPOINT &&
            ((uintptr_t)h->obj->payload & ~(uintptr_t)1) == (uintptr_t)c) err = CM_ERR_INVAL;
        q.caps[i] = *h;
        q.msg.handles[i] = 0; /* receiver-local IDs are assigned only at recv */
    }
    if (err) { cm_cap_unlock(flags); return err; }
    /* Queue + sender removal is one transaction: refs move, no retain/drop. */
    remote->slots[remote->tail] = q;
    remote->tail = (remote->tail + 1) % CM_CHAN_DEPTH;
    remote->count++;
    for (unsigned i = 0; i < msg->nhandles; i++)
        memset(&sender->handles[msg->handles[i]], 0, sizeof(cm_handle_t));
    cm_thread_t *w = remote->recv_waiter;
    remote->recv_waiter = NULL;
    cm_id_t channel_id = c->id;
    cm_obj_retain_locked(endpoint); /* keep channel alive across direct handoff */
    cm_cap_unlock(flags);
    TRACE_IPC_SEND(q.msg.trace,channel_id,q.msg.opcode,q.msg.len);
    cm_obs_emit(q.msg.trace, CM_EV_IPC_SEND, channel_id, q.msg.opcode, q.msg.len);
    if (w) {
        cm_obs_emit(q.msg.trace, CM_EV_IPC_HANDOFF, channel_id, w->id, 0);
        cm_thread_wake(w);
        cm_thread_t *cur = cm_sched_current();
        if (direct && cur && cur != w) cm_arch_ctx_swap(cur->ctx_slot, w->ctx_slot);
    }
    cm_obj_release(endpoint);
    return CM_OK;
}

static cm_error_t recv_message(cm_task_t *receiver, int hid, cm_ipc_msg_t *out, int park, cm_id_t expected)
{
    if (!out) return CM_ERR_INVAL;
    uint64_t flags = cm_cap_lock();
    cm_channel_t *c;
    cm_object_t *endpoint;
    int side;
    cm_error_t err = endpoint_locked(receiver, hid, CM_RIGHT_READ, &c, &side, &endpoint);
    if (err) { cm_cap_unlock(flags); return err; }
    if (expected && endpoint->id != expected) { cm_cap_unlock(flags); return CM_ERR_DENIED; }
    cm_endp_t *local = local_end(c, side);
    if (local->count) {
        cm_queued_msg_t *q = &local->slots[local->head];
        int ids[CM_IPC_MAX_HANDLES];
        unsigned available = 0;
        for (int i = 1; i < CM_HANDLES_PER_TASK && available < q->msg.nhandles; i++)
            if (!receiver->handles[i].obj) ids[available++] = i;
        if (available < q->msg.nhandles) { cm_cap_unlock(flags); return CM_ERR_NOSPC; }
        cm_ipc_msg_t result = q->msg;
        for (unsigned i = 0; i < q->msg.nhandles; i++) {
            receiver->handles[ids[i]] = q->caps[i]; /* adopt queue reference */
            result.handles[i] = ids[i];
            if (ids[i] + 1 > receiver->handle_cap) receiver->handle_cap = ids[i] + 1;
        }
        memset(q, 0, sizeof(*q));
        local->head = (local->head + 1) % CM_CHAN_DEPTH;
        local->count--;
        cm_id_t channel_id = c->id;
        *out = result;
        cm_cap_unlock(flags);
        cm_thread_t *trace_current=cm_sched_current();
        if (trace_current && trace_current->task==receiver) trace_current->trace_context=result.trace;
        TRACE_IPC_RECV(result.trace,channel_id,result.opcode,result.len);
        cm_obs_emit(result.trace, CM_EV_IPC_RECV, channel_id, result.opcode, result.len);
        return CM_OK;
    }
    if (c->closed[side ^ 1]) { cm_cap_unlock(flags); return CM_ERR_DEAD; }
    if (!park) { cm_cap_unlock(flags); return CM_ERR_AGAIN; }
    cm_thread_t *cur = cm_sched_current();
    if (cur) {
        if (local->recv_waiter && local->recv_waiter != cur) {
            cm_cap_unlock(flags); return CM_ERR_BUSY;
        }
        local->recv_waiter = cur;
        cm_thread_block_prepare(cur, NULL);
    }
    /* No queue/cap lock crosses a context switch. Keep local IRQs masked
     * until the prepared wait has yielded; a remote wake remains runnable. */
    cm_cap_unlock(0);
    if (cur) cm_arch_ctx_swap(cur->ctx_slot, -1);
    cm_irq_restore(flags);
    return CM_ERR_AGAIN;
}

cm_error_t cm_ipc_send(cm_task_t *sender, int hid, const cm_ipc_msg_t *msg)
{ return send_message(sender, hid, msg, 1); }
cm_error_t cm_ipc_send_deferred(cm_task_t *sender, int hid, const cm_ipc_msg_t *msg)
{ return send_message(sender, hid, msg, 0); }
cm_error_t cm_ipc_recv(cm_task_t *receiver, int hid, cm_ipc_msg_t *out)
{ return recv_message(receiver, hid, out, 1, 0); }
cm_error_t cm_ipc_try_recv(cm_task_t *receiver, int hid, cm_ipc_msg_t *out)
{ return recv_message(receiver, hid, out, 0, 0); }

cm_error_t cm_ipc_try_recv_object(cm_task_t *receiver, int hid, cm_id_t object, cm_ipc_msg_t *out)
{ return recv_message(receiver, hid, out, 0, object); }

cm_error_t cm_ipc_export_sender(cm_task_t *owner, int send_hid, int recv_hid,
                                cm_task_t *broker, int *out_hid, cm_id_t *recv_object)
{
    if (!broker || !out_hid || !recv_object) return CM_ERR_INVAL;
    uint64_t flags = cm_cap_lock();
    cm_channel_t *a, *b;
    cm_object_t *send_obj, *recv_obj;
    int sa, sb;
    cm_error_t err = endpoint_locked(owner, send_hid, CM_RIGHT_WRITE, &a, &sa, &send_obj);
    if (!err) err = endpoint_locked(owner, recv_hid, CM_RIGHT_READ, &b, &sb, &recv_obj);
    if (!err && (a != b || sa == sb)) err = CM_ERR_INVAL;
    cm_handle_t *send = cm_handle_lookup(owner, send_hid);
    if (!err && !(send->rights & CM_RIGHT_DUPLICATE)) err = CM_ERR_DENIED;
    if (!err) {
        int n = cm_handle_install_locked(broker, send_obj, CM_RIGHT_WRITE);
        if (n < 0) err = n;
        else { *out_hid = n; *recv_object = recv_obj->id; }
    }
    cm_cap_unlock(flags);
    return err;
}

cm_error_t cm_ipc_call(cm_task_t *sender, int hid,
                       const cm_ipc_msg_t *req, cm_ipc_msg_t *reply)
{
    cm_error_t e = cm_ipc_send(sender, hid, req);
    if (e != CM_OK) return e;
    return cm_ipc_recv(sender, hid, reply);
}

uint64_t cm_ipc_bench_rtt(int rounds)
{
    /* Microbench: two endpoints in the same task, ping-pong in-process.
     * This is the skeleton-liveness metric. */
    cm_task_t *t = cm_task_create("ipc-bench", CM_QOS_USER_INTERACTIVE);
    int a = -1, b = -1;
    if (cm_channel_create(t, &a, &b) != CM_OK) return 0;

    cm_ipc_msg_t ping, pong;
    memset(&ping, 0, sizeof(ping));
    ping.opcode = 0xC0DE;
    ping.len = 8;
    ping.data[0] = 1;

    uint64_t t0 = cm_rdtsc();
    for (int i = 0; i < rounds; i++) {
        if (cm_obs_enabled()) ping.trace = cm_trace_new();
        ping.data[0] = (uint8_t)i;
        if (cm_ipc_send(t, a, &ping) != CM_OK) break;
        if (cm_ipc_recv(t, b, &pong) != CM_OK) break;
        /* echo */
        if (cm_ipc_send(t, b, &pong) != CM_OK) break;
        if (cm_ipc_recv(t, a, &pong) != CM_OK) break;
    }
    uint64_t t1 = cm_rdtsc();
    uint64_t total = t1 - t0;
    g_rtt_cycles_acc += total;
    g_rtt_samples += (uint64_t)rounds;
    uint64_t per = rounds ? total / (uint64_t)rounds : 0;
    kinfo("ipc: in-process memcpy baseline %d rounds = %llu cycles/rtt (NOT L4 switch cost)",
          rounds, (unsigned long)per);
    return per;
}

static cm_task_t *g_sw_task;
static int        g_sw_a, g_sw_b;
#if 0 /* dead: run_switched — bench uses run_direct */
static int        g_sw_left;
static int        g_sw_pong_dead;
static uint64_t   g_rt_sum, g_rt_n;
static int        g_rt_armed;

static void sw_ping_fn(void *arg)
{
    CM_UNUSED(arg);
    cm_ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.opcode = 0xC0DE;
    m.len = 8;

    if (g_rt_armed) {
        if (cm_ipc_recv(g_sw_task, g_sw_a, &m) == CM_OK) {
            uint64_t t_send = 0;
            memcpy(&t_send, m.data, 8);
            g_rt_sum += cm_rdtsc() - t_send;
            g_rt_n++;
            g_rt_armed = 0;
            if (--g_sw_left <= 0) {
                cm_thread_t *th = cm_sched_current();
                if (th) th->state = CM_TH_DEAD;
            }
        }
        return;
    }

    if (g_sw_left <= 0) {
        cm_thread_t *th = cm_sched_current();
        if (th) th->state = CM_TH_DEAD;
        return;
    }

    if (cm_obs_enabled()) m.trace = cm_trace_new();
    uint64_t now = cm_rdtsc();
    memcpy(m.data, &now, 8);
    if (cm_ipc_send(g_sw_task, g_sw_a, &m) == CM_OK)
        g_rt_armed = 1;
}

static void sw_pong_fn(void *arg)
{
    CM_UNUSED(arg);
    if (g_sw_left <= 0) {
        g_sw_pong_dead = 1;
        cm_thread_t *th = cm_sched_current();
        if (th) th->state = CM_TH_DEAD;
        return;
    }
    cm_ipc_msg_t m;
    if (cm_ipc_recv(g_sw_task, g_sw_b, &m) == CM_OK)
        (void)cm_ipc_send(g_sw_task, g_sw_b, &m);
}

static uint64_t run_switched(int rounds)
{
    g_sw_left = rounds;
    g_sw_pong_dead = 0;
    g_rt_sum = g_rt_n = 0;
    g_rt_armed = 0;
    cm_thread_t *ping = cm_thread_create(g_sw_task, sw_ping_fn, NULL, CM_QOS_USER_INTERACTIVE);
    cm_thread_t *pong = cm_thread_create(g_sw_task, sw_pong_fn, NULL, CM_QOS_USER_INTERACTIVE);
    if (!ping || !pong)
        return 0;

#ifndef CAPMIX_HOSTED
    uint64_t cr3_a = 0, cr3_b = 0;
    uint64_t *sa = cm_space_create(&cr3_a);
    uint64_t *sb = cm_space_create(&cr3_b);
    if (sa && sb && cr3_a && cr3_b) {
        cm_arch_ctx_set_cr3(ping->ctx_slot, cr3_a);
        cm_arch_ctx_set_cr3(pong->ctx_slot, cr3_b);
    }
#endif

    uint64_t t0 = cm_rdtsc();
    (void)cm_sched_run_until_idle(rounds * 8 + 32);
    uint64_t t1 = cm_rdtsc();
#ifndef CAPMIX_HOSTED
    {
        uint64_t home = cm_paging_cr3();
        if (home)
            cm_space_load(home);
    }
#endif
    int done = rounds - g_sw_left;
    uint64_t wall = done > 0 ? (t1 - t0) / (uint64_t)done : 0;
    uint64_t per = g_rt_n ? g_rt_sum / g_rt_n : wall;
    kinfo("ipc: switched wall=%llu ping-timed=%llu n=%llu (prefer ping-timed)",
          (unsigned long)wall, (unsigned long)per, (unsigned long)g_rt_n);
    return per;
}
#endif /* run_switched dead */

static uint64_t run_direct(int rounds)
{
    uint64_t cr3_a = 0, cr3_b = 0;
    uint64_t *sa = cm_space_create(&cr3_a);
    uint64_t *sb = cm_space_create(&cr3_b);
    uint64_t home = cm_paging_cr3();
    if (!sa || !sb || !cr3_a || !cr3_b) {
        kinfo("ipc: direct cross-AS skipped (no spaces)");
        return 0;
    }

    cm_ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.opcode = 0xC0DE;
    m.len = 8;

    uint64_t sum = 0, n = 0;
    for (int i = 0; i < rounds; i++) {
        uint64_t t0 = cm_rdtsc();
        if (cm_obs_enabled()) m.trace = cm_trace_new();
        if (cm_ipc_send(g_sw_task, g_sw_a, &m) != CM_OK) break;
        cm_space_load(cr3_b);
        if (cm_ipc_recv(g_sw_task, g_sw_b, &m) != CM_OK) break;
        if (cm_ipc_send(g_sw_task, g_sw_b, &m) != CM_OK) break;
        cm_space_load(cr3_a);
        if (cm_ipc_recv(g_sw_task, g_sw_a, &m) != CM_OK) break;
        sum += cm_rdtsc() - t0;
        n++;
    }
    if (home)
        cm_space_load(home);
    for (int i = 0; i < CM_MAX_CHANNELS; i++) {
        if (!g_chans[i].live) continue;
        g_chans[i].a.recv_waiter = NULL;
        g_chans[i].b.recv_waiter = NULL;
        g_chans[i].a.send_waiter = NULL;
        g_chans[i].b.send_waiter = NULL;
    }
    kinfo("ipc: direct cross-AS n=%llu", (unsigned long)n);
    return n ? sum / n : 0;
}

uint64_t cm_ipc_bench_rtt_switched(int rounds)
{
    g_sw_task = cm_task_create("ipc-sw", CM_QOS_USER_INTERACTIVE);
    g_sw_a = g_sw_b = -1;
    if (!g_sw_task || cm_channel_create(g_sw_task, &g_sw_a, &g_sw_b) != CM_OK)
        return 0;

    uint64_t with_tr = run_direct(rounds);
    kinfo("ipc: cross-AS rtt trace=on  %llu cycles/rtt", (unsigned long)with_tr);

    int saved = cm_obs_enabled();
    cm_obs_enable(0);
    uint64_t quiet = run_direct(rounds);
    cm_obs_enable(saved);
    kinfo("ipc: cross-AS rtt trace=off %llu cycles/rtt (2x CR3 + 4x IPC, no sched)",
          (unsigned long)quiet);
    if (with_tr && quiet)
        kinfo("ipc: obs tax ~%lld cycles/rtt", (long)with_tr - (long)quiet);
    return quiet ? quiet : with_tr;
}

void cm_ipc_dump(void)
{
    int live = 0;
    for (int i = 0; i < CM_MAX_CHANNELS; i++) if (g_chans[i].live) live++;
    kinfo("ipc: %d live channels, rtt_samples=%llu", live, (unsigned long)g_rtt_samples);
}

cm_error_t cm_ipc_wait_prepare(cm_task_t *task,int hid,cm_id_t expected,
                              cm_thread_t *thread,cm_object_t **held,bool *parked)
{
    if (!task || !thread || thread->task!=task || !held || !parked) return CM_ERR_INVAL;
    *held=NULL; *parked=false;
    uint64_t flags=cm_cap_lock(); cm_channel_t *c; cm_object_t *endpoint; int side;
    cm_error_t err=endpoint_locked(task,hid,CM_RIGHT_READ,&c,&side,&endpoint);
    if (!err && endpoint->id!=expected) err=CM_ERR_DENIED;
    if (!err) {
        cm_endp_t *local=local_end(c,side);
        if (local->count) { /* caller retries dequeue without sleeping */ }
        else if (c->closed[side^1]) err=CM_ERR_DEAD;
        else if (local->recv_waiter && local->recv_waiter!=thread) err=CM_ERR_BUSY;
        else {
            cm_obj_retain_locked(endpoint); *held=endpoint;
            local->recv_waiter=thread;
            cm_thread_block_prepare(thread,NULL); *parked=true;
        }
    }
    cm_cap_unlock(flags); return err;
}
void cm_ipc_wait_cancel(cm_object_t *held,cm_thread_t *thread,cm_id_t thread_id,bool wake)
{
    if (!held || !thread) return;
    uint64_t flags=cm_cap_lock();
    uintptr_t tag=(uintptr_t)held->payload;
    cm_channel_t *c=(cm_channel_t *)(tag&~(uintptr_t)1);
    cm_endp_t *local=local_end(c,(int)(tag&1));
    if (local->recv_waiter==thread) local->recv_waiter=NULL;
    bool live=thread->id==thread_id && thread->state!=CM_TH_DEAD;
    cm_cap_unlock(flags);
    if (wake && live && thread->state==CM_TH_BLOCKED) cm_thread_wake(thread);
}
