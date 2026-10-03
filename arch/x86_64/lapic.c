#include "capmix/arch.h"
#include "capmix/config.h"      /* CM_MAX_CPUS 的唯一来源 */
#include "capmix/klog.h"
#include "capmix/sched.h"


#define CM_LAPIC_VIRT 0xffffffff8fee0000ull
#define MSR_APIC_BASE 0x1B
#define MSR_TSC_DEADLINE 0x6E0
#define CM_TIMER_VEC 0x40

#define CM_TICK_NS 1000000ull          /* 1 ms target period */
#define CM_PIT_HZ     1193182ull
#define CM_PIT_COUNT  11932u
#define CM_PIT_CH2    0x42u
#define CM_PIT_CMD    0x43u
#define CM_PIT_GATE   0x61u
#define CM_PERIODIC_INIT 0x10000u

static volatile uint32_t *g_lapic;
static volatile uint64_t g_timer_ticks[CM_MAX_CPUS];
static volatile int      g_need_resched[CM_MAX_CPUS];
static int      g_lapic_ready;
static int      g_deadline;

static uint64_t g_tsc_freq;
static uint64_t g_tick_tsc;
static const char *g_tsc_src = "none";

/* SMP map. g_cpu_map_ready gates cm_cpu_id(): before the LAPIC is mapped
 * (cm_gdt_load runs before paging sets up the LAPIC window) reading the
 * LAPIC ID register would fault, so we return 0 — which is the BSP. */
static int      g_cpu_count = 1;
static uint32_t g_cpu_lapic_id[CM_MAX_CPUS];
static int      g_cpu_map_ready;






static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static uint32_t lapic_read(uint32_t off)
{
    return g_lapic ? g_lapic[off / 4] : 0;
}

static void lapic_write(uint32_t off, uint32_t v)
{
    if (g_lapic)
        g_lapic[off / 4] = v;
}

uint32_t cm_lapic_id(void)
{
    return lapic_read(0x20) >> 24;
}

int cm_cpu_id(void)
{
    if (!g_cpu_map_ready) return 0;
    uint32_t id = cm_lapic_id();
    for (int i = 0; i < g_cpu_count; i++)
        if (g_cpu_lapic_id[i] == id) return i;
    return 0;
}

int cm_cpu_count(void) { return g_cpu_count; }

uint64_t cm_timer_ticks(void) { return g_timer_ticks[cm_cpu_id()]; }
int      cm_need_resched(void) { return g_need_resched[cm_cpu_id()]; }
void     cm_need_resched_clear(void) { g_need_resched[cm_cpu_id()] = 0; }

static uint64_t pit_run(uint32_t count)
{
    uint8_t gate = inb(CM_PIT_GATE);
    outb(CM_PIT_GATE, (uint8_t)((gate & ~0x02u) | 0x01u));

    outb(CM_PIT_CMD, 0xB0);
    outb(CM_PIT_CH2, (uint8_t)(count & 0xFF));
    outb(CM_PIT_CH2, (uint8_t)((count >> 8) & 0xFF));

    uint64_t t0 = cm_rdtsc();
    uint32_t spins = 0;
    int ok = 0;
    for (;;) {
        outb(CM_PIT_CMD, 0x80);
        uint32_t lo = inb(CM_PIT_CH2);
        uint32_t hi = inb(CM_PIT_CH2);
        uint32_t cur = lo | (hi << 8);
        if (cur == 0 || cur > count) { ok = 1; break; }
        if (++spins > 5000000u) break;
    }
    uint64_t t1 = cm_rdtsc();

    outb(CM_PIT_GATE, gate);
    if (!ok || t1 <= t0) return 0;
    return t1 - t0;
}

static uint64_t tsc_freq_cpuid(void)
{
    uint32_t mx = 0, a = 0, b = 0, c = 0, d = 0;
    cpuid(0, &mx, &b, &c, &d);

    if (mx >= 0x16) {
        cpuid(0x16, &a, &b, &c, &d);
        if (a >= 100 && a <= 10000)
            return (uint64_t)a * 1000000ull;
    }
    if (mx >= 0x15) {
        cpuid(0x15, &a, &b, &c, &d);
        if (a && b && c)
            return ((uint64_t)c * (uint64_t)b) / (uint64_t)a;
    }
    return 0;
}

static uint64_t tsc_freq_pit(void)
{
    uint64_t d = pit_run(CM_PIT_COUNT);
    if (!d) return 0;
    return d * CM_PIT_HZ / CM_PIT_COUNT;
}

static uint64_t detect_tsc_freq(void)
{
    uint64_t f = tsc_freq_cpuid();
    if (f) { g_tsc_src = "cpuid"; return f; }

    f = tsc_freq_pit();
    if (f >= 100000000ull && f <= 20000000000ull) {
        g_tsc_src = "pit";
        return f;
    }
    g_tsc_src = "unknown";
    return 0;
}
void cm_lapic_init_ap(void);
void cm_ap_main(int cpu);
int  cm_ap_index_of(uint32_t lapic_id);

void cm_lapic_eoi(void)
{
    lapic_write(0xB0, 0);
}

void cm_timer_arm(void)
{
    if (!g_lapic_ready) return;
    if (g_deadline && g_tick_tsc)
        wrmsr(MSR_TSC_DEADLINE, cm_rdtsc() + g_tick_tsc);
}

#define CM_TICK_PERIOD_LOG_N 5

static uint64_t g_tick_sample_tsc[CM_TICK_PERIOD_LOG_N];
static uint64_t g_tick_sample_n[CM_TICK_PERIOD_LOG_N];
static volatile int g_tick_samples_logged;

void cm_timer_isr(void)
{
    uint64_t now = cm_rdtsc();
    int cpu = cm_cpu_id();

    g_timer_ticks[cpu]++;
    g_need_resched[cpu] = 1;

    if (cpu == 0 && g_tick_samples_logged < CM_TICK_PERIOD_LOG_N) {
        g_tick_sample_tsc[g_tick_samples_logged] = now;
        g_tick_sample_n[g_tick_samples_logged]   = g_timer_ticks[0];
        g_tick_samples_logged++;
    }

    cm_lapic_eoi();
    cm_timer_arm();
    cm_sched_preempt_now();
}

void cm_timer_dump_early_ticks(void)
{
    int n = g_tick_samples_logged;
    if (n == 0) {
        kinfo("timer: no ticks recorded yet");
        return;
    }
    for (int i = 0; i < n; i++) {
        if (i == 0) {
            kinfo("timer: tick=%llu tsc=%llx (baseline, no delta yet)",
                  (unsigned long)g_tick_sample_n[i],
                  (unsigned long)g_tick_sample_tsc[i]);
            continue;
        }
        uint64_t d = g_tick_sample_tsc[i] - g_tick_sample_tsc[i - 1];
        if (g_tsc_freq)
            kinfo("timer: tick=%llu tsc=%llx delta_tsc=%llu (~%llu us, target %llu us)",
                  (unsigned long)g_tick_sample_n[i],
                  (unsigned long)g_tick_sample_tsc[i],
                  (unsigned long)d,
                  (unsigned long)(d * 1000000ull / g_tsc_freq),
                  (unsigned long)(CM_TICK_NS / 1000ull));
        else
            kinfo("timer: tick=%llu tsc=%llx delta_tsc=%llu (no tsc_freq to convert)",
                  (unsigned long)g_tick_sample_n[i],
                  (unsigned long)g_tick_sample_tsc[i],
                  (unsigned long)d);
    }
}

uint32_t cm_lapic_ccr(void)
{
    return lapic_read(0x390);
}

uint64_t cm_tsc_deadline(void)
{
    return g_deadline ? rdmsr(MSR_TSC_DEADLINE) : 0;
}

void cm_lapic_map_before_cr3(void)
{
#ifndef CAPMIX_HOSTED
    uint64_t base = rdmsr(MSR_APIC_BASE) & 0xFFFFFFFFF000ull;
    if (!base) base = 0xFEE00000ull;
    if (cm_space_map_flags(cm_paging_pml4(), CM_LAPIC_VIRT, base, 1) != 0)
        kwarn("lapic: map failed phys=%llx", (unsigned long)base);
    else
        kinfo("lapic: mapped phys=%llx -> %llx (PCD)",
              (unsigned long)base, (unsigned long)CM_LAPIC_VIRT);
#endif
}

void cm_smp_probe(void)
{
#ifdef CAPMIX_HOSTED
    kinfo("smp: skipped on hosted");
    return;
#else
    /* Any fault in here is currently INVISIBLE: cm_fault_install() runs
     * later (inside cm_irq_enable), so vectors 8/10/11/12/13/14/17 still
     * point at cm_isr_stub, which iretq's straight back into the faulting
     * instruction — a silent infinite loop that prints nothing. Install the
     * reporting stubs first so a mistake here reports instead of hanging. */
    cm_fault_install();
    kinfo("smp: probe enter cpu_id=%d", cm_cpu_id());

    uint64_t n = 0;
    uint32_t bsp = 0;
    uint32_t ids[CM_MAX_CPUS];
    for (int i = 0; i < CM_MAX_CPUS; i++) ids[i] = 0;

    int rc = cm_limine_mp(&n, &bsp, ids, CM_MAX_CPUS);
    kinfo("smp: cm_limine_mp rc=%d n=%llu bsp=%u",
          rc, (unsigned long)n, (unsigned)bsp);

    if (rc != 0 || n == 0) {
        kinfo("smp: no usable MP table, single CPU");
        g_cpu_lapic_id[0] = cm_lapic_id();
        g_cpu_count = 1;
        g_cpu_map_ready = 1;
        return;
    }

    if (n > CM_MAX_CPUS) {
        kwarn("smp: %llu CPUs present but CM_MAX_CPUS=%d — extra CPUs ignored",
              (unsigned long)n, CM_MAX_CPUS);
        n = CM_MAX_CPUS;
    }

    g_cpu_count = (int)n;
    for (uint64_t i = 0; i < n; i++)
        g_cpu_lapic_id[i] = ids[i];
    g_cpu_map_ready = 1;

    kinfo("smp: cpu_count=%d bsp_lapic_id=%u (this cpu=%u, index=%d)",
          g_cpu_count, (unsigned)bsp,
          (unsigned)cm_lapic_id(), cm_cpu_id());
    for (int i = 0; i < g_cpu_count; i++)
        kinfo("smp:   cpu[%d] lapic_id=%u%s", i, (unsigned)g_cpu_lapic_id[i],
              g_cpu_lapic_id[i] == bsp ? " (BSP)" : "");
    for (int i = 0; i < g_cpu_count; i++) {
        if (g_cpu_lapic_id[i] > 255) {
            kwarn("smp: cpu[%d] lapic_id=%u looks bogus — struct layout suspect",
                  i, (unsigned)g_cpu_lapic_id[i]);
            break;
        }
    }
#endif
}
/* --- AP bring-up -------------------------------------------------------
 * Limine starts an AP by having us WRITE the entry address into
 * info->goto_address (an atomic store; it is NOT a call). The AP arrives
 * in long mode on LIMINE's page tables with Limine's own 64 KiB stack,
 * and &info in RDI.
 *
 * That stack is the problem: it lives at an HHDM address our own tables
 * may not map, so we cannot load our CR3 while still running on it. The
 * kernel BSS, by contrast, is mapped by both page tables. So the order is
 * fixed: read our cpu index out of info (no MMIO needed), switch to a BSS
 * stack, THEN load our CR3, and only afterwards touch the LAPIC. */
__attribute__((aligned(16)))
static uint8_t  g_ap_stacks[CM_MAX_CPUS][16384];
static volatile int      g_ap_sched_enable;
static volatile int      g_ap_up[CM_MAX_CPUS];
static volatile uint64_t g_ap_loops[CM_MAX_CPUS];

int cm_ap_index_of(uint32_t lapic_id)
{
    for (int i = 0; i < g_cpu_count; i++)
        if (g_cpu_lapic_id[i] == lapic_id) return i;
    return 0;
}

__attribute__((used, naked)) void cm_ap_entry(void)
{
    /* Basic asm (no operand list), so registers are written %rax, not %%rax. */
    __asm__ volatile(
        "movq %rdi, %r15\n\t"                  /* r15 = &info               */
        "movl 4(%r15), %edi\n\t"               /* info->lapic_id            */
        "call cm_ap_index_of\n\t"              /* eax = cpu index           */
        "movl %eax, %r14d\n\t"                 /* r14 = index (callee-saved) */
        "leaq g_ap_stacks(%rip), %rax\n\t"
        "movq %r14, %rcx\n\t"
        "addq $1, %rcx\n\t"
        "shlq $14, %rcx\n\t"                   /* 16384 bytes per stack     */
        "addq %rcx, %rax\n\t"
        "andq $-16, %rax\n\t"
        "movq %rax, %rsp\n\t"                  /* now on our own BSS stack  */
        "xorl %ebp, %ebp\n\t"
        "call cm_paging_cr3\n\t"
        "movq %rax, %cr3\n\t"                  /* now on OUR page tables    */
        "movl %r14d, %edi\n\t"
        "call cm_ap_main\n\t"
        "1:\n\t"
        "hlt\n\t"
        "jmp 1b\n\t"
    );
}
void cm_ap_main(int cpu)
{
    extern void cm_pcpu_init(int cpu, uint64_t kstack_top);
    {
        uint64_t sp;
        __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
        cm_pcpu_init(cpu, sp);
    }
#ifndef CAPMIX_HOSTED
    /* Per-CPU GDT + TSS, then the shared IDT. */
    cm_idt_load_ap(cpu);
    cm_paging_enable_pge();
    cm_lapic_init_ap();

    __atomic_store_n(&g_ap_up[cpu], 1, __ATOMIC_RELEASE);
    kinfo("ap: cpu=%d lapic_id=%u up on stack %p",
          cpu, (unsigned)cm_lapic_id(), (void *)&g_ap_stacks[cpu][16384]);

    /* Take interrupts from here on. The AP's own LAPIC timer has to be able
     * to land, otherwise a thread this CPU swapped into could never be
     * preempted and would run until it chose to yield. */
    __asm__ volatile("sti");

    /* Gate: g_ap_sched_enable stays 0 through every selftest. Those tests
     * assert on exact scheduler counts (bg_work=27, run_ticks=8, steps=16),
     * and an AP picking threads concurrently changes all of them. The
     * stress phase flips it on once nothing else is asserting. */
    for (;;) {
        g_ap_loops[cpu]++;
        if (__atomic_load_n(&g_ap_sched_enable, __ATOMIC_ACQUIRE) &&
            cm_sched_runnable() > 0) {
            /* Same entry as the BSP, so an AP goes through the identical
             * pick + claim + BKL path. Returns when the thread yields or
             * this CPU's own tick preempts it. */
            cm_sched_tick();
        } else {
            /* Parked: drop the claim on the last thread we ran, so that
             * cm_sched_drain() can reap it. Otherwise every parked AP
             * pins one leftover thread in the runqueue forever. */
            cm_sched_release_current();
            __asm__ volatile("pause" ::: "memory");
        }
    }
#else
    (void)cpu;   /* hosted never starts an AP; just satisfy the linker */
#endif
}

int cm_smp_online(void)
{
    int n = 1;
    for (int i = 0; i < g_cpu_count; i++)
        if (__atomic_load_n(&g_ap_up[i], __ATOMIC_ACQUIRE)) n++;
    return n;
}

void cm_smp_start(void)
{
#ifdef CAPMIX_HOSTED
    kinfo("smp: start skipped on hosted");
    return;
#else
    if (g_cpu_count <= 1) {
        kinfo("smp: only the BSP — no APs to start");
        return;
    }
    int started = 0;
    for (int i = 1; i < g_cpu_count; i++) {
        if (cm_limine_mp_start((uint64_t)i, (void *)cm_ap_entry) != 0) {
            kwarn("smp: cannot start cpu[%d]", i);
            continue;
        }
        uint64_t spins = 0;
        while (!__atomic_load_n(&g_ap_up[i], __ATOMIC_ACQUIRE)) {
            if (++spins > 200000000ull) break;   /* bounded: never hang boot */
            __asm__ volatile("pause" ::: "memory");
        }
        if (__atomic_load_n(&g_ap_up[i], __ATOMIC_ACQUIRE)) started++;
        else kwarn("smp: cpu[%d] did not report up", i);
    }
    kinfo("smp: started %d AP(s), online=%d/%d", started, cm_smp_online(), g_cpu_count);
#endif
}

void cm_smp_dump(void)
{
    kinfo("smp: online=%d/%d  (BSP + APs)", cm_smp_online(), g_cpu_count);
    for (int i = 0; i < g_cpu_count; i++) {
        kinfo("  cpu[%d] lapic_id=%u up=%d loops=%llu ticks=%llu "
              "preempts=%llu stack_top=%p",
              i, (unsigned)g_cpu_lapic_id[i],
              __atomic_load_n(&g_ap_up[i], __ATOMIC_ACQUIRE),
              (unsigned long)__atomic_load_n(&g_ap_loops[i], __ATOMIC_ACQUIRE),
              (unsigned long)g_timer_ticks[i],
              (unsigned long)cm_sched_preempts_cpu(i),
              (void *)&g_ap_stacks[i][16384]);
    }
}
uint64_t cm_timer_ticks_cpu(int cpu)
{
    if (cpu < 0 || cpu >= CM_MAX_CPUS) return 0;
    return g_timer_ticks[cpu];
}

void cm_smp_sched_enable(void) { __atomic_store_n(&g_ap_sched_enable, 1, __ATOMIC_RELEASE); }
void cm_smp_sched_disable(void){ __atomic_store_n(&g_ap_sched_enable, 0, __ATOMIC_RELEASE); }

/* SMP scheduler stress — the first thing that actually contends the BKL.
 *
 * Runs AFTER every other selftest on purpose: while they execute the APs
 * are held out of the scheduler, because their picks would move exactly the
 * counts those tests assert on. */
/* --- SMP scheduler stress --------------------------------------------
 * The first thing that actually contends the BKL: every CPU runs the same
 * pick + claim + accounting path over the same runqueue.
 *
 * Runs AFTER every other selftest on purpose — those assert on exact
 * scheduler counts, and APs picking concurrently would move all of them. */
#define CM_STRESS_THREADS 8

static cm_task_t   *g_stress_task;
static uint64_t     g_stress_count[CM_STRESS_THREADS];
static cm_thread_t *g_stress_th[CM_STRESS_THREADS];

static void stress_fn(void *arg)
{
    uint64_t *n = (uint64_t *)arg;

    /* Short burst, then RETURN — that is what makes this a lock stress
     * rather than a throughput test.
     *
     * Returning drops through landing() -> cm_arch_ctx_swap(tid,-1) ->
     * cm_sched_tick(), which re-picks and re-takes the BKL. So every burst
     * is one more contended acquisition. A thread that spun forever would
     * instead be PREEMPTED, and the preemption path deliberately does NOT
     * take the lock — it would contribute nothing to lock_contend. */
    for (volatile int i = 0; i < 100; i++)
        __asm__ volatile("pause" ::: "memory");

    /* Non-atomic on purpose: two CPUs can hold the same thread only if the
     * scheduler picked it twice, which the RUNNING filter forbids. This is
     * a progress counter, not an assertion. */
    if (n) (*n)++;
}

void cm_smp_sched_stress(uint64_t rounds)
{
#ifdef CAPMIX_HOSTED
    (void)rounds;
    kinfo("smp: sched stress skipped on hosted");
    return;
#else
    /* 1. Park the APs FIRST. cm_sched_drain() cannot reap a thread another
     * CPU is swapped into, so they must be out of the scheduler — and must
     * have run cm_sched_release_current() — before we clear the queue.
     * Release the BSP's own claim too: preempt_selftest left g_current[0]
     * pointing at one of its hogs. */
    cm_smp_sched_disable();
    cm_sched_release_current();
    for (uint64_t i = 0; i < 5000000ull; i++)
        __asm__ volatile("pause" ::: "memory");

    /* 2. Clean runqueue, so lock_contend measures THIS load and not the
     * selftests' leftovers (which included a driver the isolation test had
     * already killed). */
    int drained = cm_sched_drain();
    
    /* 3. Defined load: N threads, same QoS, same weight, each doing a
     * short burst then returning to the scheduler. */
    g_stress_task = cm_task_create("smp-stress", CM_QOS_USER_INTERACTIVE);
    if (!g_stress_task) {
        kinfo("smp: sched stress skipped — no task slot (drained=%d)", drained);
        return;
    }
    int made = 0;
    for (int i = 0; i < CM_STRESS_THREADS; i++) {
        g_stress_count[i] = 0;
        g_stress_th[i] = cm_thread_create(g_stress_task, stress_fn,
                                          &g_stress_count[i],
                                          CM_QOS_USER_INTERACTIVE);
        if (!g_stress_th[i]) break;
        made++;
    }
    if (made == 0) {
        kinfo("smp: sched stress skipped — no thread slots");
        return;
    }

    uint64_t t0[CM_MAX_CPUS], p0[CM_MAX_CPUS];
    for (int i = 0; i < g_cpu_count; i++) {
        t0[i] = g_timer_ticks[i];
        p0[i] = cm_sched_preempts_cpu(i);
    }
    uint64_t lc0 = cm_sched_lock_contend();

    kinfo("smp: sched stress begin online=%d/%d drained=%d threads=%d rounds=%llu",
          cm_smp_online(), g_cpu_count, drained, made, (unsigned long)rounds);

    /* 4. Release the APs AND run the BSP through the same loop. Contended
     * from both sides is the only way lock_contend means anything — with
     * only the BSP scheduling, contend stays 0 no matter how bad the lock
     * is. */
    cm_smp_sched_enable();
    for (uint64_t i = 0; i < rounds; i++) {
        if (cm_sched_runnable() > 0)
            cm_sched_tick();
        else
            __asm__ volatile("pause" ::: "memory");
    }
    /* Park them again so the numbers below are stable while we read them. */
    cm_smp_sched_disable();
    for (uint64_t i = 0; i < 5000000ull; i++)
        __asm__ volatile("pause" ::: "memory");

    /* 5. Report. Leave the stress threads in place: cm_sched_dump() right
     * after this shows their per-thread run_ticks, which is the
     * load-balance check — a lock that serialises badly shows up as wildly
     * uneven ticks across the 8 threads. */
    kinfo("smp: sched stress end preempts=%llu lock_contend +%llu",
          (unsigned long)cm_sched_preempts(),
          (unsigned long)(cm_sched_lock_contend() - lc0));
    for (int i = 0; i < g_cpu_count; i++)
        kinfo("smp:   cpu[%d] ticks +%llu preempts +%llu",
              i,
              (unsigned long)(g_timer_ticks[i] - t0[i]),
              (unsigned long)(cm_sched_preempts_cpu(i) - p0[i]));
    {
        uint64_t total = 0;
        for (int i = 0; i < made; i++) total += g_stress_count[i];
        kinfo("smp: stress threads completed %llu bursts "
              "(each burst = one contended pick)",
              (unsigned long)total);
    }
#endif
}

/* AP-only LAPIC setup. BSP-only work is deliberately absent: no PIT
 * calibration (four CPUs poking the PIT at once yields garbage, and
 * g_tsc_freq is already known), no PIC masking. */
void cm_lapic_init_ap(void)
{
#ifndef CAPMIX_HOSTED
    g_lapic = (volatile uint32_t *)CM_LAPIC_VIRT;
    uint64_t apic = rdmsr(MSR_APIC_BASE);
    wrmsr(MSR_APIC_BASE, apic | (1ull << 11));   /* software enable */
    lapic_write(0xF0, 0x1FF);                    /* spurious vector */
    lapic_write(0x80, 0);                        /* TPR */
    lapic_write(0x350, 1u << 16);                /* mask LINT0 */
    lapic_write(0x360, 1u << 16);                /* mask LINT1 */
    lapic_write(0x370, 1u << 16);                /* mask error    */

    /* Timer: reuse the BSP's calibrated g_tick_tsc. Never re-run the PIT
     * here — four CPUs hammering PIT channel 2 at once gives garbage, and
     * the BSP already measured the TSC frequency before any AP existed.
     * MSR_TSC_DEADLINE and every LAPIC register are per-CPU, so this arms
     * only this AP. */
    if (g_deadline && g_tick_tsc) {
        lapic_write(0x320, CM_TIMER_VEC | 0x40000u);
        wrmsr(MSR_TSC_DEADLINE, cm_rdtsc() + g_tick_tsc);
        kinfo("ap: cpu=%d lapic timer armed tick_tsc=%llu",
              cm_cpu_id(), (unsigned long)g_tick_tsc);
    } else {
        kwarn("ap: cpu=%d no TSC-deadline mode — timer NOT armed", cm_cpu_id());
    }
#endif
}

void cm_lapic_init(void)
{
#ifdef CAPMIX_HOSTED
    kinfo("lapic: skipped on hosted");
    return;
#else
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    int has_apic = (d >> 9) & 1;
    int has_deadline = (c >> 24) & 1;
    kinfo("lapic: cpuid1 edx.apic=%d ecx.tsc_deadline=%d", has_apic, has_deadline);

    g_lapic = (volatile uint32_t *)CM_LAPIC_VIRT;
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    uint64_t apic = rdmsr(MSR_APIC_BASE);
    wrmsr(MSR_APIC_BASE, apic | (1ull << 11));
    lapic_write(0xF0, 0x1FF);
    lapic_write(0x80, 0);
    lapic_write(0x350, 1u << 16);
    lapic_write(0x360, 1u << 16);
    lapic_write(0x370, 1u << 16);

    g_tsc_freq = detect_tsc_freq();
    kinfo("lapic: tsc_freq=%llu Hz (%llu MHz, src=%s)",
          (unsigned long)g_tsc_freq,
          (unsigned long)(g_tsc_freq / 1000000ull),
          g_tsc_src);

    if (has_deadline && g_tsc_freq) {
        g_deadline = 1;
        g_tick_tsc = g_tsc_freq * CM_TICK_NS / 1000000000ull;
        if (!g_tick_tsc) g_tick_tsc = 3000000ull;
        lapic_write(0x320, CM_TIMER_VEC | 0x40000u);
        g_lapic_ready = 1;
        kinfo("lapic: TSC-deadline tick_tsc=%llu (target %llu ns), armed after sti",
              (unsigned long)g_tick_tsc, (unsigned long)CM_TICK_NS);
    } else {
        g_deadline = 0;
        g_tick_tsc = 0;
        lapic_write(0x3E0, 0x3);
        lapic_write(0x320, CM_TIMER_VEC | 0x20000u);
        lapic_write(0x380, CM_PERIODIC_INIT);
        g_lapic_ready = 1;
        kwarn("lapic: periodic UNCALIBRATED - APIC bus assumed 1 GHz");
        kinfo("lapic: periodic init=%x lvt=%x id=%x ver=%x svr=%x",
              CM_PERIODIC_INIT, lapic_read(0x320),
              lapic_read(0x20) >> 24, lapic_read(0x30), lapic_read(0xF0));
    }

    kinfo("lapic: readback lvt(0x320)=%x dcr(0x3E0)=%x",
          lapic_read(0x320), lapic_read(0x3E0));
#endif
}

void cm_irq_enable(void)
{
#ifndef CAPMIX_HOSTED
    cm_fault_install();
    cm_fault_dump_gate(CM_TIMER_VEC);

    {
        uint64_t sp;
        __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
        kinfo("probe rsp=%llx (boot_stack=%p) pre-sti",
              (unsigned long)sp, cm_boot_stack_base());
    }
    kinfo("lapic: pre-sti lvt=%x id=%x svr=%x",
          lapic_read(0x320), lapic_read(0x20), lapic_read(0xF0));

    __asm__ volatile("sti");

    cm_timer_arm();
    kinfo("lapic: armed dl=%llx tsc=%llx (dl>tsc=%d)",
          (unsigned long)cm_tsc_deadline(),
          (unsigned long)cm_rdtsc(),
          (int)(cm_tsc_deadline() > cm_rdtsc()));

    for (uint64_t i = 0; i < 200000ull; i++)
        __asm__ volatile("pause");
    {
        uint64_t rflags;
        __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
        kinfo("irq: after bare-spin IF=%d firings=%llu ticks=%llu ccr=%x",
              (int)((rflags >> 9) & 1),
              (unsigned long)cm_idt_firings(),
              (unsigned long)g_timer_ticks[0],
              (unsigned)cm_lapic_ccr());
    }
    if (g_timer_ticks[0] == 0)
        kwarn("timer: zero ticks after spin — deadline likely never armed");
    cm_timer_dump_early_ticks();
#endif
}