#include "capmix/trace.h"
#include "capmix/sched.h"
#include "capmix/arch.h"
#include "capmix/mm.h"
#include "capmix/numa.h"
#include "capmix/klog.h"
#include "capmix/obs.h"
#include "capmix/config.h"
#include "capmix/proc.h"

#include <string.h>

static cm_task_t    g_tasks[CM_MAX_TASKS];
static cm_thread_t  g_threads[CM_MAX_THREADS];
static cm_thread_t *g_rq[CM_QOS_NBANDS][CM_MAX_THREADS];
static int          g_rq_len[CM_QOS_NBANDS];

/* Per-CPU "who is running here". Was a single global: with 4 CPUs that is
 * four threads all believing they are g_current, and cm_sched_preempt_now()
 * would happily swap the wrong one out. */
static cm_thread_t *g_current[CM_MAX_CPUS];

static cm_id_t      g_tid_next = 1;
static cm_id_t      g_task_next = 1;
static int64_t      g_min_vruntime;
static uint64_t     g_aging[CM_QOS_NBANDS];

/* Per-CPU, same reason as g_current: g_in_thread gates preemption, so a
 * global would let CPU 2's tick preempt CPU 0's thread. */
static int          g_in_thread[CM_MAX_CPUS];
static uint64_t     g_preempt_count[CM_MAX_CPUS];

/* Big kernel lock for the scheduler.
 *
 * Held ONLY across pick + claim + accounting — never across
 * cm_arch_ctx_swap(). That call does not return until the thread yields or
 * is preempted, i.e. a full time slice; and an AP with no timer armed
 * might never return at all. Holding the lock across it would stall every
 * other CPU indefinitely.
 *
 * The ISR path (cm_sched_preempt_now) deliberately does NOT take this: it
 * can fire on a CPU that already owns the lock, and a self-take there
 * deadlocks instantly. */
static volatile unsigned int g_rq_lock;
static uint64_t              g_lock_contend;
static void (*g_poll_hook)(void);
void cm_sched_set_poll_hook(void (*hook)(void))
{__atomic_store_n(&g_poll_hook,hook,__ATOMIC_RELEASE);}

static void sched_lock(void)
{
    if (__sync_lock_test_and_set(&g_rq_lock, 1u) == 0u) return;
    __sync_fetch_and_add(&g_lock_contend, 1ull);
    for (;;) {
        __asm__ volatile("pause" ::: "memory");
        if (__sync_lock_test_and_set(&g_rq_lock, 1u) == 0u) return;
    }
}

static void sched_unlock(void)
{
    __sync_lock_release(&g_rq_lock);
}

static int qos_weight(cm_qos_t q)
{
    static const int w[] = { 2048, 1536, 1024, 768, 512, 256 };
    return w[(unsigned)q < CM_QOS_NBANDS ? q : CM_QOS_DEFAULT];
}

void cm_sched_init(void)
{
    memset(g_tasks, 0, sizeof(g_tasks));
    memset(g_threads, 0, sizeof(g_threads));
    memset(g_rq, 0, sizeof(g_rq));
    memset(g_rq_len, 0, sizeof(g_rq_len));
    memset(g_aging, 0, sizeof(g_aging));
    memset(g_current, 0, sizeof(g_current));
    memset(g_in_thread, 0, sizeof(g_in_thread));
    memset(g_preempt_count, 0, sizeof(g_preempt_count));
    g_tid_next = 1;
    g_task_next = 1;
    g_min_vruntime = 0;
    g_rq_lock = 0;
    g_lock_contend = 0;
    g_poll_hook = NULL;
}

/* Per-CPU accessors run with IF=0: cm_cpu_id() and the array access must
 * happen on the same CPU, and a thread can be preempted and migrated in
 * between. See cm_irq_save() in arch.h. */
cm_thread_t *cm_sched_current(void)
{
    uint64_t fl = cm_irq_save();
    cm_thread_t *th = g_current[cm_cpu_id()];
    cm_irq_restore(fl);
    return th;
}
void cm_sched_set_current(cm_thread_t *th)
{
    uint64_t fl = cm_irq_save();
    g_current[cm_cpu_id()] = th;
    cm_irq_restore(fl);
}
int cm_sched_in_thread(void)
{
    uint64_t fl = cm_irq_save();
    int r = g_in_thread[cm_cpu_id()];
    cm_irq_restore(fl);
    return r;
}

/* Summed across CPUs: the selftest compares this against a single-CPU
 * expectation, and per-CPU counters are the only thing that stays correct
 * once more than one CPU is scheduling. */
uint64_t     cm_sched_preempts(void)
{
    uint64_t n = 0;
    for (int i = 0; i < CM_MAX_CPUS; i++) n += g_preempt_count[i];
    return n;
}

int cm_sched_runnable(void)
{
    cm_linux_futex_poll();
    void (*hook)(void)=__atomic_load_n(&g_poll_hook,__ATOMIC_ACQUIRE);
    if(hook)hook(); /* no rq lock held; may enqueue a deferred sleeper */
    int n = 0;
    for (int q = 0; q < CM_QOS_NBANDS; q++) n += g_rq_len[q];
    return n;
}

uint64_t cm_sched_preempts_cpu(int cpu)
{
    if (cpu < 0 || cpu >= CM_MAX_CPUS) return 0;
    return g_preempt_count[cpu];
}

uint64_t cm_sched_lock_contend(void) { return g_lock_contend; }

/* True if any CPU other than `self` is currently swapped into th.
 *
 * Must be called with the BKL held: g_current[] is written under the lock,
 * so this is the only way to ask "is anyone else running this thread?"
 * without a window in which the answer goes stale.
 *
 * DEFINED HERE, not further down: cm_sched_drain() below calls it, and a
 * use before the definition makes GCC treat it as implicitly declared
 * (int-returning, unchecked args) and then reject the real static
 * definition with "static declaration follows non-static declaration". */
static int thread_busy_elsewhere(cm_thread_t *th, int self)
{
    for (int i = 0; i < CM_MAX_CPUS; i++)
        if (i != self && g_current[i] == th) return 1;
    return 0;
}

/* Called by a CPU that has stopped scheduling: drop our claim on the last
 * thread we ran. Without this, g_current[cpu] keeps pointing at that thread
 * forever, and cm_sched_drain() — which refuses to reap a thread another
 * CPU still holds — would silently leave up to one leftover per parked CPU
 * in the runqueue. */
void cm_sched_release_current(void)
{
    g_current[cm_cpu_id()] = NULL;
}

/* Mark every live thread DEAD and reap it, leaving an empty runqueue.
 *
 * Used by the SMP stress phase to get a CLEAN runqueue. Before this, the
 * APs were picking the selftests' leftovers — including com.capmix.blockd,
 * which the driver-isolation test already declared dead — so lock_contend
 * was measuring "4 CPUs fighting over a handful of arbitrary leftovers"
 * rather than a defined load. A number measured on an undefined load
 * cannot be compared between runs.
 *
 * Returns how many were actually reaped. Anything another CPU is still
 * swapped into is left behind (cm_thread_reap refuses it), so park the APs
 * and let them call cm_sched_release_current() before draining. */
int cm_sched_drain(void)
{
    int n = 0;
    for (int i = 0; i < CM_MAX_THREADS; i++) {
        if (g_threads[i].id == 0) continue;

        /* Ask "is another CPU in this thread?" under the BKL, exactly like
         * cm_sched_tick() does. g_current[] is only stable while the lock is
         * held; cm_thread_reap() re-checks, but by then we have already
         * marked the thread DEAD, and a DEAD thread some CPU is still
         * running on is what produced the earlier cr2=0x65 fault.
         *
         * Decide under the lock, act outside it: cm_thread_reap() calls
         * rq_remove(), which takes the BKL itself — calling it while we hold
         * the lock self-deadlocks (sched_lock() spins unbounded). */
        sched_lock();
        int busy = thread_busy_elsewhere(&g_threads[i], cm_cpu_id());
        if (!busy) g_threads[i].state = CM_TH_DEAD;
        sched_unlock();
        if (busy) continue;

        cm_thread_reap(&g_threads[i]);
        if (g_threads[i].id == 0) n++;   /* cm_thread_reap memsets on success */
    }
    return n;
}

/* A live thread satisfies &g_threads[th->ctx_slot] == th exactly; see
 * cm_thread_create(). A reaped thread is memset to 0, so its ctx_slot is 0
 * and the identity fails for every slot but 0.
 *
 * Catches the stale-pointer case a range check misses: a pointer into
 * recycled memory whose ctx_slot is still numerically plausible. */
static int thread_slot_sane(cm_thread_t *th)
{
    /* WARN-ONLY. Always returns 1, so this is a pure observer: pick keeps
     * every candidate and cm_sched_tick never takes its early return. The
     * identity it checks is still exact for any thread built by
     * cm_thread_create() (ctx_slot = th - g_threads), so a mismatch means a
     * use-after-reap or a thread constructed by some other path — reported
     * once by name instead of silently corrupting the run. */
    if (!th || th->id == 0) return 1;
    if (th->ctx_slot < 0 || th->ctx_slot >= CM_MAX_THREADS) return 1;
    if (&g_threads[th->ctx_slot] != th) {
        static volatile int warned;
        if (!__sync_lock_test_and_set(&warned, 1))
            kerr("sched: stale thread %p id=%llu ctx_slot=%d "
                 "(&g_threads[%d]=%p) — use after reap?",
                 (void *)th, (unsigned long)th->id, th->ctx_slot,
                 th->ctx_slot, (void *)&g_threads[th->ctx_slot]);
    }
    return 1;
}

const char *cm_qos_name(cm_qos_t q)
{
    static const char *n[] = {
        "UserInteractive", "UserInitiated", "Default",
        "Utility", "Background", "Idle"
    };
    return (unsigned)q < CM_QOS_NBANDS ? n[q] : "?";
}

static cm_task_t *alloc_task(void)
{
    for (int i = 0; i < CM_MAX_TASKS; i++)
        if (g_tasks[i].id == 0) return &g_tasks[i];
    return NULL;
}

static cm_thread_t *alloc_thread(void)
{
    uint64_t flags=cm_irq_save();sched_lock();
    cm_thread_t *th=NULL;
    for(int i=0;i<CM_MAX_THREADS;i++)if(!g_threads[i].id) {
        th=&g_threads[i];memset(th,0,sizeof(*th));
        th->id=g_tid_next++;break; /* reserve before an AP can recycle it */
    }
    sched_unlock();cm_irq_restore(flags);return th;
}

cm_task_t *cm_task_create(const char *name, cm_qos_t qos)
{
    cm_task_t *t = alloc_task();
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->id = g_task_next++;
    t->name = name ? name : "task";
    t->qos = qos;
    t->oom_score_adj = 0;
    t->jetsam_band = (uint32_t)qos;
    t->self_obj = cm_obj_mint(CM_OBJ_TASK, t, CM_RIGHT_ALL);
    return t;
}

static void rq_push(cm_thread_t *th)
{
    cm_qos_t q = th->qos;
    if (th->rq_index >= 0) return;
    sched_lock();
    int n = g_rq_len[q];
    g_rq[q][n] = th;
    th->rq_index = n;
    g_rq_len[q] = n + 1;
    sched_unlock();
    th->state = CM_TH_RUNNABLE;
    CM_TRACE(SCHED_ENQUEUE,th->trace_context,th->id,q,0);
}

static void rq_remove(cm_thread_t *th)
{
    if (th->rq_index < 0) return;
    cm_qos_t q = th->qos;
    sched_lock();
    int i = th->rq_index;
    int last = g_rq_len[q] - 1;
    if (i != last) {
        g_rq[q][i] = g_rq[q][last];
        g_rq[q][i]->rq_index = i;
    }
    g_rq_len[q]--;
    sched_unlock();
    th->rq_index = -1;
    CM_TRACE(SCHED_DEQUEUE,th->trace_context,th->id,q,0);
}

cm_thread_t *cm_thread_create(cm_task_t *task, cm_th_entry_t entry, void *arg, cm_qos_t qos)
{
    if(!task)return NULL;
    cm_thread_t *th = alloc_thread();
    if (!th) return NULL;
    th->task = task;
    th->entry = entry;
    th->arg = arg;
    th->qos = qos;
    th->weight = qos_weight(qos);
    th->slice = 4;
    th->vruntime = g_min_vruntime;
    th->ve = g_min_vruntime;
    th->vdeadline = th->ve + (th->slice * 1024) / th->weight;
    th->lag = 0;
    th->rq_index = -1;
    th->ctx_slot = (int)(th - g_threads);
    /* 2 pages, matching the 2 * CM_PAGE_SIZE handed to ctx_init below.
     * Allocating 1 while placing the stack top at +8192 put every thread's
     * first 4 KiB of stack growth inside a page the buddy never handed us
     * — it only survived because the neighbour happened to be unused. */
    th->kstack = cm_page_alloc_node(task ? task->memory_node : 0, 2);
    if (!th->kstack) { th->id = 0; return NULL; }
    if (cm_arch_ctx_init(th->ctx_slot, entry, arg, th->kstack, 2 * CM_PAGE_SIZE) != 0) {
        cm_page_free(th->kstack, 2);
        th->kstack = NULL;
        th->id = 0;
        return NULL;
    }
    th->state = CM_TH_RUNNABLE;
    rq_push(th);
    return th;
}

void cm_sched_yield(void)
{
    cm_thread_t *th = cm_sched_current();
    if (!th) return;
    cm_arch_ctx_swap(th->ctx_slot, -1);
}

void cm_thread_block_prepare(cm_thread_t *th, cm_thread_t *on)
{
    if (!th) return;
    rq_remove(th);
    th->state = CM_TH_BLOCKED;
    th->blocked_on = on;
    if (on) cm_turnstile_propagate(th);
}

void cm_thread_block(cm_thread_t *th, cm_thread_t *on)
{
    if (!th) return;
    cm_thread_block_prepare(th, on);
    if (th == cm_sched_current()) cm_arch_ctx_swap(th->ctx_slot, -1);
}

/* Caller must NOT hold the BKL (this calls rq_remove, which takes it). */
void cm_thread_reap(cm_thread_t *th)
{
    if (!th || th->id == 0) return;

    /* Never free a thread another CPU is still swapped into.
     *
     * This is the cr2=0x65 fault: one CPU marked a thread DEAD and reaped
     * it (cm_page_free(kstack) + memset(slot)), while another CPU picked
     * that same thread and cm_arch_ctx_swap()'d onto the now-freed stack.
     * The next kinfo from that thread dereferenced a va_list living in
     * recycled memory and got a garbage %s pointer.
     *
     * Our own thread is fine to reap — we only get here after
     * cm_arch_ctx_swap() has returned control to us.
     *
     * TWO ORDERING FIXES, both needed for the cr2=0x96 fault:
     *
     * (1) The busy check now runs UNDER the BKL. It used to read
     *     g_current[] unlocked, contradicting the contract stated above
     *     thread_busy_elsewhere (g_current[] is only stable under the
     *     lock). Unlocked, another CPU could set g_current[i]=th between
     *     the check and the free below, and swap onto a stack that was
     *     already handed back to the buddy.
     *
     * (2) Detaching g_current[] now happens BEFORE cm_page_free(), not
     *     after. The old order left a window in which the kstack was
     *     already recycled while some CPU still advertised this thread as
     *     current — and the reaped thread's slot was still pickable
     *     through it. */
    sched_lock();
    if (thread_busy_elsewhere(th, cm_cpu_id())) { sched_unlock(); return; }
    for (int i = 0; i < CM_MAX_CPUS; i++)
        if (g_current[i] == th) g_current[i] = NULL;
    sched_unlock();

    rq_remove(th);
    if (th->kstack) { cm_page_free(th->kstack, 2); th->kstack = NULL; }

    /* Invalidate the switch slot BEFORE memset() zeroes th->ctx_slot.
     * g_ctx[] is a separate array indexed by slot and kept ready=1 with an
     * rsp pointing into the kstack we just freed. Once that page is
     * reallocated two contexts share one stack — which is how an interrupt
     * frame ends up pushed at rsp=0, faulting with cr2=0xfffffffffffffff8. */
    cm_arch_ctx_clear(th->ctx_slot);

    /* Publish an empty slot atomically with allocator reservation. */
    uint64_t flags=cm_irq_save();sched_lock();
    memset(th,0,sizeof(*th));
    sched_unlock();cm_irq_restore(flags);
}

void cm_thread_wake(cm_thread_t *th)
{
    if (!th || th->state == CM_TH_DEAD) return;
    th->blocked_on = NULL;
    if (th->pi_boost) {
        th->qos = th->task ? th->task->qos : th->qos;
        th->pi_boost = 0;
    }
    th->state = CM_TH_RUNNABLE;
    rq_push(th);
}

void cm_turnstile_propagate(cm_thread_t *waiter)
{
    cm_thread_t *w = waiter;
    int hops = 0;
    while (w && w->blocked_on && hops++ < 16) {
        cm_thread_t *h = w->blocked_on;
        if (h->qos > w->qos) {
            cm_qos_t old = h->qos;
            rq_remove(h);
            h->qos = w->qos;
            h->pi_boost = 1;
            h->weight = qos_weight(h->qos);
            if (h->state == CM_TH_RUNNABLE || h->state == CM_TH_RUNNING) rq_push(h);
            if (cm_obs_enabled()) cm_obs_emit(cm_trace_new(), CM_EV_TURNSTILE, w->id, h->id, h->qos);
            if (cm_obs_enabled()) cm_obs_emit(cm_trace_new(), CM_EV_QOS_BOOST, h->id, old, h->qos);
        }
        w = h;
    }
}

static void lag_rezero_band(int q)
{
    int64_t minl = 0;
    int any = 0;
    for (int i = 0; i < g_rq_len[q]; i++) {
        if (!any || g_rq[q][i]->lag < minl) minl = g_rq[q][i]->lag;
        any = 1;
    }
    if (!any || minl >= 0) return;
    for (int i = 0; i < g_rq_len[q]; i++) g_rq[q][i]->lag -= minl;
}

cm_thread_t *cm_eevdf_pick(void)
{
    for (int q = 1; q < CM_QOS_NBANDS; q++) {
        int higher = 0;
        for (int h = 0; h < q; h++) higher += g_rq_len[h];
        if (g_rq_len[q] > 0 && higher > 0) g_aging[q]++;
        else g_aging[q] = 0;
    }

    for (int q = 0; q < CM_QOS_NBANDS; q++) {
        cm_thread_t *cand[CM_MAX_THREADS];
        int nc = 0;

        /* Skip threads we must not enter:
         *
         *  RUNNING = some CPU is already swapped into it. Without this two
         *           CPUs can pick the SAME thread and both swap onto its
         *           stack.
         *  DEAD    = it is being (or has been) reaped. Its kstack may
         *           already be back in the buddy; entering it means running
         *           on recycled memory.
         *
         * Wake may publish RUNNABLE before blocker's context save completes.
         * The old CPU claim remains authoritative until it is back on its
         * scheduler stack; checking state alone lets two CPUs enter one stack.
         * Check claims for both ordinary and aging candidates.
         *
         * Single-CPU behaviour is unchanged: cm_sched_tick() resets its
         * own previous thread to RUNNABLE after the swap returns, so it
         * stays eligible on the next pick. */
        for (int i = 0; i < g_rq_len[q] && nc < CM_MAX_THREADS; i++)
            if (g_rq[q][i]->state != CM_TH_RUNNING &&
                g_rq[q][i]->state != CM_TH_DEAD &&
                !thread_busy_elsewhere(g_rq[q][i],cm_cpu_id()) &&
                cm_proc_task_cpu_allowed(g_rq[q][i]->task, cm_cpu_id()) &&
                thread_slot_sane(g_rq[q][i])) cand[nc++] = g_rq[q][i];

        for (int q2 = q + 1; q2 < CM_QOS_NBANDS; q2++) {
            if (g_aging[q2] <= 64) continue;
            for (int i = 0; i < g_rq_len[q2] && nc < CM_MAX_THREADS; i++)
                if (g_rq[q2][i]->state != CM_TH_RUNNING &&
                    g_rq[q2][i]->state != CM_TH_DEAD &&
                    !thread_busy_elsewhere(g_rq[q2][i],cm_cpu_id()) &&
                    cm_proc_task_cpu_allowed(g_rq[q2][i]->task, cm_cpu_id()) &&
                    thread_slot_sane(g_rq[q2][i])) cand[nc++] = g_rq[q2][i];
        }
        if (nc == 0) continue;

        cm_thread_t *best = NULL;
        for (int i = 0; i < nc; i++) {
            if (cand[i]->lag < 0) continue;
            if (!best || cand[i]->vdeadline < best->vdeadline) best = cand[i];
        }
        if (!best) {
            for (int qz = q; qz < CM_QOS_NBANDS; qz++) lag_rezero_band(qz);
            for (int i = 0; i < nc; i++) {
                if (cand[i]->lag < 0) continue;
                if (!best || cand[i]->vdeadline < best->vdeadline) best = cand[i];
            }
        }
        if (!best) {
            best = cand[0];
            for (int i = 1; i < nc; i++)
                if (cand[i]->vdeadline < best->vdeadline) best = cand[i];
        }
        if (cm_obs_enabled()) cm_obs_emit(cm_trace_new(), CM_EV_SCHED_PICK, best->id, best->vdeadline, q);
        return best;
    }
    return NULL;
}

void cm_sched_tick(void)
{
    int cpu = cm_cpu_id();

    /* cli around the critical section. A timer ISR landing between
     * sched_lock() and sched_unlock() would run cm_sched_preempt_now() and
     * swap away while this CPU still owns the lock — the lock would stay
     * held until some other CPU's tick happened to return through here.
     *
     * Hosted MUST NOT execute this: cli is privileged, and the hosted
     * build is a ring-3 Linux process, so it faults immediately. There is
     * no ISR to race against there anyway. The declaration is inside the
     * #ifndef too, so hosted does not trip -Wunused-variable either. */
#ifndef CAPMIX_HOSTED
    uint64_t flags = 0;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif

    sched_lock();

    cm_thread_t *th = cm_eevdf_pick();
    if (!th) {
        g_current[cpu] = NULL;
        sched_unlock();
#ifndef CAPMIX_HOSTED
        if (flags & 0x200) __asm__ volatile("sti");
#endif
        return;
    }
    cm_thread_t *prev = g_current[cpu];

    /* Last line of defence before the swap. Reaching here means pick
     * returned a pointer whose ctx_slot is inconsistent with its own
     * address — a use-after-reap we would otherwise chase as a #PF deep
     * inside cm_arch_ctx_swap. Print once and drop the pick instead. */
    if (!thread_slot_sane(th)) {
        static volatile int warned_slot;
        if (!__sync_lock_test_and_set(&warned_slot, 1))
            kerr("sched: pick returned stale thread %p id=%llu ctx_slot=%d "
                 "(&g_threads[%d]=%p) — use after reap?",
                 (void *)th, (unsigned long)th->id, th->ctx_slot,
                 th->ctx_slot, (void *)&g_threads[th->ctx_slot]);
        g_current[cpu] = NULL;
        sched_unlock();
#ifndef CAPMIX_HOSTED
        if (flags & 0x200) __asm__ volatile("sti");
#endif
        return;
    }

    g_current[cpu] = th;
    th->state = CM_TH_RUNNING;
    th->run_ticks++;

    int64_t dt_v = (1024 * 1) / (th->weight ? th->weight : 1024);
    if (dt_v < 1) dt_v = 1;
    th->vruntime += dt_v;
    th->lag -= dt_v;
    th->vdeadline = th->vruntime + (th->slice * 1024) / th->weight;

    /* One walk, not two: lag accounting and the min_vruntime scan iterate
     * the same runqueue, so fold them together.
     *
     * This is INSIDE the BKL and it is O(all runnable threads). With four
     * CPUs spinning on the lock, walking the queue twice doubles the
     * window everybody else waits through. Same result, half the work. */
    int64_t minvr = th->vruntime;
    for (int q = 0; q < CM_QOS_NBANDS; q++) {
        for (int i = 0; i < g_rq_len[q]; i++) {
            cm_thread_t *o = g_rq[q][i];
            if (o != th) o->lag += dt_v;
            if (o->vruntime < minvr) minvr = o->vruntime;
        }
    }
    g_min_vruntime = minvr;

    /* Released BEFORE the swap: cm_arch_ctx_swap() does not come back
     * until the thread yields or is preempted. */
    sched_unlock();
#ifndef CAPMIX_HOSTED
    if (flags & 0x200) __asm__ volatile("sti");
#endif

    if (prev && prev != th) {
        TRACE_SCHED_SWITCH(th->trace_context,prev->id,th->id,th->qos);
        if (cm_obs_enabled()) cm_obs_emit(cm_trace_new(), CM_EV_SCHED_SWITCH, prev->id, th->id, th->qos);
    }

#ifndef CAPMIX_HOSTED
    /* Interrupts stay off across g_in_thread=1 AND the swap.
     *
     * g_current[cpu] is already th at this point, so a tick landing in
     * this window passes every check in cm_sched_preempt_now() and runs
     * cm_arch_ctx_swap(th->ctx_slot, -1): it stores the SCHEDULER's rsp
     * (the AP stack, just after this call) as the thread's context and
     * then resumes the scheduler at the frame cm_sched_tick() was in one
     * tick ago, whose `th` and `cpu` are stale. That is where the two
     * remaining faults come from — th==NULL read at offset 0x10
     * (cr2=0x10) and a wild write through g_current[cpu]
     * (cr2=0xfffffffb80186d38) — and it leaves two live contexts stacked
     * on one kernel stack, which later unwinds into an iretq that pops
     * someone else's frame (#GP, err=0).
     *
     * "sti before g_in_thread=1" only moves the window: with
     * g_in_thread==0 the tick no-ops, but it still lands between sti and
     * the swap. Closing it is the only fix.
     *
     * The incoming thread's IF now comes from its saved rflags instead of
     * from whatever the CPU was running with: fresh threads start at
     * 0x202, and a preempted one resumes with IF=0 and gets IF=1 back
     * from its own iretq. */
    __asm__ volatile("cli");
#endif

    g_in_thread[cpu] = 1;
    cm_arch_ctx_swap(-1, th->ctx_slot);
    g_in_thread[cpu] = 0;

#ifndef CAPMIX_HOSTED
    /* Coming back from a preemption we resume with IF=0 (no iretq ran);
     * coming back from a cooperative yield IF is already 1. */
    __asm__ volatile("sti");
#endif

    /* This CPU owns the stopped context; CR3 is back on the scheduler's
     * kernel view. A ring3 victim holds no kernel resources on its suspended
     * ISR frame. Syscall continuations must resume and unwind instead. */
    cm_proc_t *victim=cm_proc_current();
    if(victim && victim->elf && !victim->exited && !victim->linux_thread_dead && victim->elf_started && !__atomic_load_n(&victim->linux_migrate,__ATOMIC_ACQUIRE) &&
       __atomic_load_n(&victim->kill_pending,__ATOMIC_ACQUIRE) &&
       !__atomic_load_n(&victim->syscall_depth,__ATOMIC_ACQUIRE)) {
        cm_proc_finish_exit(victim,137);
        kinfo("user kill retired: pid=%llu cpu=%d off-cpu=1 exit=137",(unsigned long)victim->pid,cpu);
    }

    /* Decide under the lock, act OUTSIDE it.
     *
     * thread_busy_elsewhere() reads g_current[], which is only stable while
     * we hold the BKL — but cm_thread_reap() calls rq_remove(), which takes
     * the BKL ITSELF. Calling it from in here self-deadlocks: sched_lock()
     * is an UNBOUNDED spin, so re-acquiring a lock this CPU already holds
     * spins forever. That is what hung the IPC selftest — it is the first
     * test where threads exit normally (state -> DEAD), so it was the first
     * to reach this branch. */
    sched_lock();
    if(cm_proc_migrate_stopped(th))g_current[cpu]=NULL;
    int dead = (th->state == CM_TH_DEAD);
    int busy = dead ? thread_busy_elsewhere(th, cpu) : 0;
    if (!dead && th->state == CM_TH_RUNNING)
        th->state = CM_TH_RUNNABLE;
    sched_unlock();

    if (dead && !busy)
        cm_thread_reap(th);
}

/* Called from the timer ISR. The interrupted thread's full state lives on
 * its own stack (volatile regs pushed by cm_timer_stub + the iret frame),
 * so storing rsp captures everything. Switch back to the SCHEDULER (-1)
 * and let cm_sched_tick() re-pick, keeping one "who runs next" path.
 *
 * NO sched_lock() here on purpose — see the comment on g_rq_lock. */
void cm_sched_preempt_now(void)
{
    int cpu = cm_cpu_id();
    cm_thread_t *cur = g_current[cpu];
    if (!g_in_thread[cpu] || !cur) return;
    if (!cm_need_resched()) return;

    cm_need_resched_clear();

    /* The outgoing thread must still be the owner of its ctx slot.
     *
     * This path takes no lock, so `cur` can have been reaped by another
     * CPU between the read above and here. cm_thread_reap() memsets the
     * thread (ctx_slot -> 0) and clears g_ctx[slot]; if that slot has
     * since been handed to a freshly created thread, cm_arch_ctx_swap()
     * writes the state we are leaving into the NEW thread's context —
     * rsp/rip pointing into a kstack that is already freed. When that
     * thread is next entered it unwinds into recycled memory, which is
     * the shape of every remaining SMP fault (garbage `th`, garbage
     * `cpu`, iretq popping a foreign frame).
     *
     * Bail out instead of swapping: the tick is simply dropped. */
    if (cur->id == 0 || &g_threads[cur->ctx_slot] != cur ||
        !cm_arch_ctx_ready(cur->ctx_slot)) {
        g_current[cpu] = NULL;
        g_in_thread[cpu] = 0;
        return;
    }

    g_in_thread[cpu] = 0;
    g_preempt_count[cpu]++;
    cm_proc_t *p = cm_proc_current();
    if (p && p->elf && p->elf_started && !p->exited) p->elf_preempts++;

    TRACE_SCHED_SWITCH(cur->trace_context,cur->id,0,0x100);
    if (cm_obs_enabled()) cm_obs_emit(cm_trace_new(), CM_EV_SCHED_SWITCH, cur->id, 0, 0x100);
    cm_arch_ctx_swap(cur->ctx_slot, -1);
    if (cm_proc_task_is_elf(cur->task)) cm_usermode_resume_entry();
}

void cm_sched_maybe_preempt(void)
{
    if (!g_in_thread[cm_cpu_id()]) return;
    cm_sched_preempt_now();
}

int cm_sched_run_until_idle(int max_steps)
{
    int steps = 0;
    while (steps < max_steps) {
        if (!cm_sched_runnable()) break;
        cm_sched_maybe_preempt();
        cm_sched_tick();
        steps++;
    }
    g_current[cm_cpu_id()] = NULL;
    return steps;
}

void cm_sched_dump(void)
{
    int cpu = cm_cpu_id();
    kinfo("sched: min_vruntime=%lld current=%llu preempts=%llu lock_contend=%llu",
          (long)g_min_vruntime,
          (unsigned long)(g_current[cpu] ? g_current[cpu]->id : 0),
          (unsigned long)cm_sched_preempts(),
          (unsigned long)g_lock_contend);
    for (int q = 0; q < CM_QOS_NBANDS; q++) {
        if (g_rq_len[q] == 0) continue;
        kinfo("  band %s (%d threads, aging=%llu):",
              cm_qos_name((cm_qos_t)q), g_rq_len[q], (unsigned long)g_aging[q]);
        for (int i = 0; i < g_rq_len[q]; i++) {
            cm_thread_t *th = g_rq[q][i];
            kinfo("    tid=%llu vr=%lld vd=%lld lag=%lld ticks=%llu",
                  (unsigned long)th->id, (long)th->vruntime, (long)th->vdeadline,
                  (long)th->lag, (unsigned long)th->run_ticks);
        }
    }
}
static void claim_fixture(void *arg){CM_UNUSED(arg);}
int cm_sched_claim_selftest(void)
{
    /* Deterministic early-wake model: runnable, but another CPU still owns
     * its outgoing continuation. AP scheduling is parked during selftests. */
    if(cm_cpu_count()>=CM_MAX_CPUS) {
        kinfo("sched early-wake synthetic claim skipped: no offline CPU slot");return 0;
    }
    cm_task_t *task=cm_task_create("wake-claim",CM_QOS_USER_INTERACTIVE);
    if(!task)return 1;
    cm_thread_t *th=cm_thread_create(task,claim_fixture,NULL,CM_QOS_USER_INTERACTIVE);
    if(!th){cm_obj_release(task->self_obj);memset(task,0,sizeof(*task));return 1;}
    uint64_t flags=cm_irq_save();sched_lock();
    int other=CM_MAX_CPUS-1; /* offline slot cannot be cleared by an idle AP */
    th->lag=(1ll<<62);th->vdeadline=-(1ll<<62); /* wins whenever eligible */
    cm_thread_t *saved=g_current[other];g_current[other]=th;
    int failed=cm_eevdf_pick()==th;
    g_current[other]=saved;
    failed|=cm_eevdf_pick()!=th; /* now the saved context is available */
    sched_unlock();cm_irq_restore(flags);
    th->state=CM_TH_DEAD;cm_thread_reap(th);
    cm_obj_release(task->self_obj);memset(task,0,sizeof(*task));
    if(!failed)kinfo("sched early-wake claim verified: RUNNABLE on other CPU cannot enter before context handoff");
    return failed;
}
