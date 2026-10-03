#include "capmix/ipc.h"
#include "capmix/sched.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/arch.h"

#include <string.h>

/* L4-style synchronous rendezvous.
 * If a peer is already blocked in recv, we hand the message over and
 * conceptually do a direct context switch (no run-queue hop).
 * Small payloads live in the message struct ("registers").
 * Large payloads will be VMO caps (phase 3).
 */

#define CM_CHAN_DEPTH 8

typedef struct {
    cm_ipc_msg_t slots[CM_CHAN_DEPTH];
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

static cm_channel_t *chan_from_handle(cm_task_t *t, int hid, int *side)
{
    cm_handle_t *h = cm_handle_lookup(t, hid);
    if (!h || !h->obj) return NULL;
    if (h->obj->type != CM_OBJ_CHANNEL && h->obj->type != CM_OBJ_ENDPOINT)
        return NULL;
    if ((h->rights & (CM_RIGHT_READ | CM_RIGHT_WRITE)) == 0) return NULL;
    uintptr_t tag = (uintptr_t)h->obj->payload;
    int s = (int)(tag & 1);
    cm_channel_t *c = (cm_channel_t *)(tag & ~(uintptr_t)1);
    if (side) *side = s;
    return c;
}

cm_error_t cm_channel_create(cm_task_t *owner, int *out_a, int *out_b)
{
    if (!owner || !out_a || !out_b) return CM_ERR_INVAL;
    cm_channel_t *c = alloc_chan();
    if (!c) return CM_ERR_NOSPC;
    /* Two endpoint objects share the channel; low bit selects side. */
    cm_object_t *ea = cm_obj_mint(CM_OBJ_ENDPOINT, (void *)((uintptr_t)c | 0),
                                  CM_RIGHT_READ | CM_RIGHT_WRITE | CM_RIGHT_TRANSFER | CM_RIGHT_DUPLICATE);
    cm_object_t *eb = cm_obj_mint(CM_OBJ_ENDPOINT, (void *)((uintptr_t)c | 1),
                                  CM_RIGHT_READ | CM_RIGHT_WRITE | CM_RIGHT_TRANSFER | CM_RIGHT_DUPLICATE);
    if (!ea || !eb) return CM_ERR_NOMEM;
    int ha = cm_handle_install(owner, ea, ea->default_rights);
    int hb = cm_handle_install(owner, eb, eb->default_rights);
    if (ha < 0 || hb < 0) return CM_ERR_NOSPC;
    *out_a = ha;
    *out_b = hb;
    return CM_OK;
}

static cm_endp_t *local_end(cm_channel_t *c, int side)  { return side ? &c->b : &c->a; }
static cm_endp_t *remote_end(cm_channel_t *c, int side) { return side ? &c->a : &c->b; }

static int enqueue(cm_endp_t *e, const cm_ipc_msg_t *m)
{
    if (e->count >= CM_CHAN_DEPTH) return -1;
    e->slots[e->tail] = *m;
    e->tail = (e->tail + 1) % CM_CHAN_DEPTH;
    e->count++;
    return 0;
}

static int dequeue(cm_endp_t *e, cm_ipc_msg_t *m)
{
    if (e->count == 0) return -1;
    *m = e->slots[e->head];
    e->head = (e->head + 1) % CM_CHAN_DEPTH;
    e->count--;
    return 0;
}

cm_error_t cm_ipc_send(cm_task_t *sender, int hid, const cm_ipc_msg_t *msg)
{
    int side;
    cm_channel_t *c = chan_from_handle(sender, hid, &side);
    if (!c || !msg) return CM_ERR_INVAL;
    cm_endp_t *remote = remote_end(c, side);
    cm_ipc_msg_t m = *msg;
    /* cm_trace_new() costs ~458 cycles (ticks + current-thread lookup). Do not pay it when nobody is consuming: observation is a capability, not a global switch. */
    if (m.trace == 0 && cm_obs_enabled()) m.trace = cm_trace_new();
    if (m.len > CM_IPC_SMALL_BYTES) m.len = CM_IPC_SMALL_BYTES;

    cm_obs_emit(m.trace, CM_EV_IPC_SEND, c->id, m.opcode, m.len);

    if (remote->recv_waiter) {
        /* Direct handoff + conceptual process switch (L4). */
        enqueue(remote, &m);
        cm_thread_t *w = remote->recv_waiter;
        remote->recv_waiter = NULL;
        cm_obs_emit(m.trace, CM_EV_IPC_HANDOFF, c->id, w->id, 0);
        cm_thread_wake(w);
        {
            cm_thread_t *cur = cm_sched_current();
            if (cur && cur != w)
                cm_arch_ctx_swap(cur->ctx_slot, w->ctx_slot);
        }
        return CM_OK;
    }
    if (enqueue(remote, &m) != 0) return CM_ERR_AGAIN;
    return CM_OK;
}

cm_error_t cm_ipc_recv(cm_task_t *receiver, int hid, cm_ipc_msg_t *out)
{
    int side;
    cm_channel_t *c = chan_from_handle(receiver, hid, &side);
    if (!c || !out) return CM_ERR_INVAL;
    cm_endp_t *local = local_end(c, side);

    if (dequeue(local, out) == 0) {
        cm_obs_emit(out->trace, CM_EV_IPC_RECV, c->id, out->opcode, out->len);
        return CM_OK;
    }
    /* Would block: park current thread. Hosted cooperative loop treats
     * CM_ERR_AGAIN as "try after peers run". */
    cm_thread_t *cur = cm_sched_current();
    if (cur) {
        local->recv_waiter = cur;
        cm_thread_block(cur, NULL);
    }
    return CM_ERR_AGAIN;
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
