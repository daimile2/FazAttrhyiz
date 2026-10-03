#include "capmix/arch.h"
#include "capmix/config.h"
#include "capmix/klog.h"

#include <string.h>
#include <stdint.h>

extern void cm_ctx_switch_asm(void *from, void *to);

typedef struct {
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip;
    uint64_t cr3;
    int      ready;
    int      done;
} cm_ctx_t;

static uint64_t g_host_cr3;

/* Slots [0, CM_MAX_THREADS) are threads, indexed by tid — SMP-safe, because
 * a tid names one thread no matter which CPU runs it.
 *
 * Slots [CM_MAX_THREADS, CM_MAX_THREADS + CM_MAX_CPUS) are the SCHEDULER
 * context, one per CPU. This used to be a SINGLE shared slot: every CPU's
 * cm_arch_ctx_swap(-1, tid) stored its own state there, so the second CPU
 * to call it overwrote the first, and one of them resumed on the other's
 * stack. Harmless while only the BSP scheduled; fatal the moment an AP
 * enters the scheduler. */
#define CM_HOST_SLOT(cpu) (CM_MAX_THREADS + (cpu))

static cm_ctx_t g_ctx[CM_MAX_THREADS + CM_MAX_CPUS];
static void   (*g_entry[CM_MAX_THREADS])(void *);
static void    *g_arg[CM_MAX_THREADS];

static int g_running[CM_MAX_CPUS] = { [0 ... CM_MAX_CPUS - 1] = -1 };

/* Is this a stack pointer we could ever legitimately be running on?
 *
 * Kernel BSS stacks (boot, AP, IST) live at 0xffffffff80000000+; thread
 * kstacks come from the buddy at the HHDM base 0xffff800000000000+. Both
 * are in the canonical higher half, so anything below 0xffff800000000000
 * is not a stack this kernel handed out.
 *
 * This exists because of a real fault:
 *   [FAULT] vec=0xe err=0x2 rip=<cm_cpu_id> cr2=0xfffffffffffffff8
 * err=0x2 means supervisor WRITE, but the instruction at rip was a read —
 * so the write was the CPU pushing an interrupt frame, and cr2 = 0-8 says
 * RSP was 0 when it did. rflags came back as 0x13706 (TF and DF both set,
 * IOPL=3), which no kernel path ever produces: the whole frame was read
 * off a corrupted stack. Refusing to ENTER such a context turns that from
 * a fatal fault into a skipped pick plus a message naming the slot. */
static int ctx_rsp_sane(uint64_t rsp)
{
#ifdef CAPMIX_HOSTED
    /* Hosted stacks are user-space malloc/pthread stacks, nowhere near the
     * kernel half — the floor below would reject every single swap. The
     * check exists for bare metal only, where "not in the kernel half"
     * means "a stack nobody handed out". */
    (void)rsp;
    return 1;
#else
    return rsp >= 0xffff800000000000ull;
#endif
}

static void landing(void)
{
    for (;;) {
        int tid = g_running[cm_cpu_id()];
        if (tid >= 0 && tid < CM_MAX_THREADS && g_entry[tid])
            g_entry[tid](g_arg[tid]);
        cm_arch_ctx_swap(tid, -1);
    }
}

int cm_arch_ctx_init(int tid, void (*entry)(void *), void *arg, void *stack, size_t stack_sz)
{
    if (tid < 0 || tid >= CM_MAX_THREADS || !stack || stack_sz < 4096 || !entry)
        return -1;
    memset(&g_ctx[tid], 0, sizeof(g_ctx[tid]));
    uintptr_t top = ((uintptr_t)stack + stack_sz) & ~(uintptr_t)15;
    uint64_t *sp = (uint64_t *)top;
    *--sp = 0;
    g_ctx[tid].rsp = (uint64_t)(uintptr_t)sp;
    g_ctx[tid].rip = (uint64_t)(uintptr_t)landing;
    g_ctx[tid].ready = 1;
    g_ctx[tid].done = 0;
    g_entry[tid] = entry;
    g_arg[tid] = arg;
    return 0;
}

/* Invalidate a slot after its thread is reaped.
 *
 * cm_thread_reap() frees the kstack and memsets the thread struct, but
 * g_ctx[] is a SEPARATE array indexed by slot — it kept ready=1 and an rsp
 * pointing into the page we just handed back to the buddy. Once that page
 * is reallocated (which cm_sched_drain() followed by 8 cm_thread_create()
 * calls does immediately), two contexts share one stack: legal rip, garbage
 * stack pointer, interrupt frame pushed somewhere it does not belong.
 *
 * Clearing g_entry[]/g_arg[] closes the same hole on the landing() path,
 * which would otherwise call a stale entry with a stale arg. */
void cm_arch_ctx_clear(int tid)
{
    if (tid < 0 || tid >= CM_MAX_THREADS) return;
    memset(&g_ctx[tid], 0, sizeof(g_ctx[tid]));
    g_entry[tid] = NULL;
    g_arg[tid] = NULL;
}

void cm_arch_ctx_swap(int from_tid, int to_tid)
{
    int cpu = cm_cpu_id();
    if (cpu < 0 || cpu >= CM_MAX_CPUS) {
        /* Out-of-range cpu makes hs, g_running[cpu] and g_ctx[hs] all
         * out of bounds at once. Refuse rather than corrupt three things. */
        static volatile int warned_cpu;
        if (!__sync_lock_test_and_set(&warned_cpu, 1))
            kerr("ctx: refusing swap, cm_cpu_id()=%d out of range", cpu);
        return;
    }
    int hs  = CM_HOST_SLOT(cpu);
    int f = (from_tid < 0) ? hs : from_tid;
    int t = (to_tid   < 0) ? hs : to_tid;
    int last = CM_MAX_THREADS + CM_MAX_CPUS;

    /* Every index validated BEFORE g_ctx[] is touched.
     *
     * f matters as much as t: cm_ctx_switch_asm() STORES the outgoing
     * context at &g_ctx[f], so a bad f is a wild write, not a bad read. */
    if (f < 0 || f >= last || t < 0 || t >= last) {
        static volatile int warned_idx;
        if (!__sync_lock_test_and_set(&warned_idx, 1))
            kerr("ctx: refusing swap cpu=%d from=%d to=%d f=%d t=%d "
                 "(valid 0..%d) — stale thread pointer or bad slot?",
                 cpu, from_tid, to_tid, f, t, last - 1);
        return;
    }

    if (t != hs) {
        if (!g_ctx[t].ready) return;
        if (!ctx_rsp_sane(g_ctx[t].rsp)) {
            /* Print once: this is a diagnostic, not a logging site, and a
             * slot this broken would otherwise be hit on every pick. */
            static volatile int warned;
            if (!__sync_lock_test_and_set(&warned, 1))
                kerr("ctx: refusing swap into slot %d, rsp=%llx is not a "
                     "kernel stack (thread reaped but slot not cleared?)",
                     t, (unsigned long)g_ctx[t].rsp);
            return;
        }
    }

    g_ctx[hs].ready = 1;
    g_running[cpu] = (to_tid < 0) ? -1 : to_tid;
#ifndef CAPMIX_HOSTED
    {
        uint64_t want = (t == hs) ? g_host_cr3 : g_ctx[t].cr3;
        if (want)
            cm_space_load(want);
    }
#endif
    cm_ctx_switch_asm(&g_ctx[f], &g_ctx[t]);
}

void cm_arch_ctx_set_cr3(int tid, uint64_t cr3)
{
    if (tid < 0 || tid >= CM_MAX_THREADS) return;
    g_ctx[tid].cr3 = cr3;
    if (!g_host_cr3)
        g_host_cr3 = cm_paging_cr3();
}

int cm_arch_ctx_done(int tid)
{
    if (tid < 0 || tid >= CM_MAX_THREADS) return 1;
    return g_ctx[tid].done;
}