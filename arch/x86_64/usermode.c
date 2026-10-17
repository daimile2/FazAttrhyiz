#include "capmix/arch.h"
#include "capmix/syscall.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/kernel.h"
#include "capmix/config.h"
#include "capmix/elf.h"

#include <string.h>

/* NXE is per logical CPU; called by the existing per-CPU usermode init. */
int cm_arch_nx_supported(void)
{
    unsigned a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000000u),"c"(0));
    if(a<0x80000001u)return 0;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000001u),"c"(0));
    return !!(d&(1u<<20));
}

extern void     cm_syscall_entry(void);
void cm_land_user(void);
extern uint64_t cm_kern_rsp;
extern uint64_t cm_user_syscalls;
extern uint64_t cm_last_cpl;


#define USER_CS  0x2B
#define USER_SS  0x23

#define CM_PCPU_MAX 8
/* Offsets shared with syscall.S: 0 stack, 8 user rsp, 16 reserved,
 * 24 full CR3, 32 shadow CR3, 40 scratch rax. */
typedef struct { uint64_t rsp0, user_rsp, kern_rsp, full_cr3, user_cr3, scratch, incoming_cr3, bad_entries, leave_user, entry_top; } cm_pcpu_t;
_Static_assert(offsetof(cm_pcpu_t, full_cr3) == 24, "entry full CR3 offset");
_Static_assert(offsetof(cm_pcpu_t, user_cr3) == 32, "entry shadow CR3 offset");
_Static_assert(offsetof(cm_pcpu_t, scratch) == 40, "entry scratch offset");
_Static_assert(offsetof(cm_pcpu_t, incoming_cr3) == 48, "entry incoming offset");
_Static_assert(offsetof(cm_pcpu_t, bad_entries) == 56, "entry counter offset");
_Static_assert(offsetof(cm_pcpu_t, leave_user) == 64, "entry exit flag offset");
static cm_pcpu_t g_pcpu[CM_PCPU_MAX] __attribute__((section(".entry.data"), aligned(4096)));
#ifndef CAPMIX_HOSTED
static uint8_t g_entry_stacks[CM_PCPU_MAX + CM_MAX_PROCS][16384]
    __attribute__((section(".entry.data"), aligned(4096)));
extern void cm_enter_user(uint64_t cr3, uint64_t rip, uint64_t rsp, uint64_t entry_top);
#endif

void cm_usermode_resume_entry(void)
{
#ifndef CAPMIX_HOSTED
    int cpu = cm_cpu_id();
    if (g_pcpu[cpu].entry_top) cm_tss_set_rsp0(cpu,g_pcpu[cpu].entry_top);
#endif
}
void cm_usermode_exit_request(void) { g_pcpu[cm_cpu_id()].leave_user=1; }
static inline void pcpu_wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

void cm_pcpu_init(int cpu, uint64_t kstack_top)
{
    if (cpu < 0 || cpu >= CM_PCPU_MAX) return;
    g_pcpu[cpu].rsp0     = kstack_top;
    g_pcpu[cpu].kern_rsp = kstack_top;
    pcpu_wrmsr(0xC0000101,0);
    pcpu_wrmsr(0xC0000102, (uint64_t)(uintptr_t)&g_pcpu[cpu]);
}

/* Snapshot the entry ABI and both SWAPGS phases along with every context.
 * GS bases are rebound locally; explicit migration also rebuilds the kernel continuation. */
#ifndef CAPMIX_HOSTED
static struct { cm_pcpu_t p; uint64_t gs, kgs; int valid; }
    g_entry_state[CM_MAX_THREADS + CM_MAX_CPUS];
static uint64_t entry_rdmsr(uint32_t msr)
{
    uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(msr));
    return ((uint64_t)hi<<32)|lo;
}
#endif
void cm_usermode_context_clear(int slot)
{
#ifndef CAPMIX_HOSTED
    if(slot>=0 && slot<CM_MAX_THREADS) memset(&g_entry_state[slot],0,sizeof(g_entry_state[slot]));
#else
    CM_UNUSED(slot);
#endif
}
void cm_usermode_context_switch(int from,int to)
{
#ifndef CAPMIX_HOSTED
    int cpu=cm_cpu_id(),f=from<0?CM_MAX_THREADS+cpu:from,t=to<0?CM_MAX_THREADS+cpu:to;
    g_entry_state[f].p=g_pcpu[cpu];
    g_entry_state[f].gs=entry_rdmsr(0xc0000101);
    g_entry_state[f].kgs=entry_rdmsr(0xc0000102);
    g_entry_state[f].valid=1;
    if(g_entry_state[t].valid) {
        g_pcpu[cpu]=g_entry_state[t].p;
        /* Native workers may migrate; GS denotes this CPU in either phase. */
        uint64_t base=(uint64_t)(uintptr_t)&g_pcpu[cpu];
        pcpu_wrmsr(0xc0000101,g_entry_state[t].gs?base:0);
        pcpu_wrmsr(0xc0000102,g_entry_state[t].kgs?base:0);
        if(g_pcpu[cpu].entry_top) cm_tss_set_rsp0(cpu,g_pcpu[cpu].entry_top);
    } else {
        memset(&g_pcpu[cpu],0,sizeof(g_pcpu[cpu]));
        pcpu_wrmsr(0xc0000101,0);
        pcpu_wrmsr(0xc0000102,(uint64_t)(uintptr_t)&g_pcpu[cpu]);
    }
#else
    CM_UNUSED(from);CM_UNUSED(to);
#endif
}

#define USER_BASE 0x400000ull

#ifdef CAPMIX_HOSTED

void cm_usermode_run_elf(struct cm_proc *p)
{
    /* Hosted validates the loader without pretending to execute x86 ring3. */
    p->elf_started = 0;
    p->exited = 1; p->exit_code = CM_ERR_NOTSUP;
    cm_thread_t *th = cm_sched_current();
    if (th) th->state = CM_TH_DEAD;
    kinfo("exec: hosted ELF execution skipped (no ring3)");
}
int cm_kpti_active(void) { return 0; }
void cm_usermode_init(void) {}
void cm_land_user(void) {}
int  cm_usermode_selftest(void)
{
    kinfo("usermode: skipped on hosted (no ring3)");
    return 0;
}
uint64_t cm_usermode_syscalls(void) { return 0; }
uint64_t cm_usermode_cpl(void) { return 0; }
uint64_t cm_space_switch_cost(void) { return 0; }

#else

static int g_um_ready[CM_PCPU_MAX];
static int g_kpti_verified;
/* Observation only: entry ABI and CR3 transitions remain unchanged. */
int cm_kpti_active(void)
{
#if CM_KPTI
    return g_kpti_verified || (g_um_ready[0] && cm_user_syscalls && g_pcpu[0].user_cr3 &&
           g_pcpu[0].full_cr3!=g_pcpu[0].user_cr3 && !g_pcpu[0].bad_entries &&
           g_pcpu[0].incoming_cr3==g_pcpu[0].user_cr3);
#else
    return 0;
#endif
}

static uint64_t g_home_cr3;
static uint64_t g_sys_before;
static uint8_t *g_user_code;

static void wrmsr(uint32_t msr, uint64_t v)
{
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void cm_usermode_init(void)
{
    uint64_t efer = rdmsr(0xC0000080);
    wrmsr(0xC0000080, efer | 1ull | (cm_arch_nx_supported()?(1ull<<11):0));
    wrmsr(0xC0000081, (0x18ull << 48) | (0x08ull << 32));
    wrmsr(0xC0000082, (uint64_t)(uintptr_t)cm_syscall_entry);
    wrmsr(0xC0000084, 0x257fdull);
    g_um_ready[cm_cpu_id()] = 1;
    kinfo("usermode: EFER.SCE LSTAR=%p STAR user=0x18 kernel=0x08",
          (void *)cm_syscall_entry);
}

uint64_t cm_usermode_syscalls(void) { return cm_user_syscalls; }
uint64_t cm_usermode_cpl(void) { return cm_last_cpl; }

uint64_t cm_space_switch_cost(void)
{
    uint64_t p1 = 0, p2 = 0;
    uint64_t *a = cm_space_create(&p1);
    uint64_t *b = cm_space_create(&p2);
    if (!a || !b || !p1 || !p2) return 0;
    uint64_t home = cm_paging_cr3();
    uint64_t t0 = cm_rdtsc();
    for (int i = 0; i < 200; i++) {
        cm_space_load(p1);
        cm_space_load(p2);
    }
    uint64_t t1 = cm_rdtsc();
    cm_space_load(home);
    uint64_t per = (t1 - t0) / 200;
    kinfo("usermode: CR3 ping-pong 200x2 = %llu cycles/pair (TLB shootdown included)",
          (unsigned long)per);
    return per;
}

int cm_usermode_selftest(void)
{
    if (!g_um_ready[cm_cpu_id()]) cm_usermode_init();
    if (!cm_paging_pml4()) {
        kerr("usermode: no kernel pml4");
        return -1;
    }

    uint64_t cr3 = 0;
    uint64_t *pml4 = cm_space_create(&cr3);
    if (!pml4) {
        kerr("usermode: space create failed");
        return -1;
    }

    void *code = cm_page_alloc(0);
    void *stkp = cm_page_alloc(0);
    if (!code || !stkp) {
        kerr("usermode: page alloc failed");
        return -1;
    }
    memset(code, 0x90, CM_PAGE_SIZE);
    memset(stkp, 0, CM_PAGE_SIZE);
    g_user_code = (uint8_t *)code;
    g_user_code[0x90] = 0xff; /* poison: must be overwritten by user code */

    /* Report CPL to 0x400090, then write "hello", then exit.
     *  8c c8                 mov %cs, %eax
     *  83 e0 03              and $3, %eax
     *  48 ba 90 00 40 00 ... movabs $0x400090, %rdx
     *  88 02                 mov %al, (%rdx)
     *  then original write + exit blob. */
    /* End-to-end syscall cost, measured from ring3 with the real SYSCALL
     * instruction — the only number comparable to seL4's 986 cycles/rtt.
     *
     * SYSCALL clobbers rcx (return addr) and r11 (rflags), so the loop
     * counter is r12 and t0 is kept in rsi (cm_syscall_entry saves rsi).
     * Result lands at 0x400094, which cm_land_user reads back.
     */
    static const unsigned char blob[] = {
        0x8c, 0xc8,                                     /* mov %cs,%eax   */
        0x83, 0xe0, 0x03,                               /* and $3,%eax    */
        0x48, 0xba, 0x90, 0x00, 0x40, 0x00, 0x00, 0x00,
        0x00, 0x00,                                     /* movabs $0x400090,%rdx */
        0x88, 0x02,                                     /* mov %al,(%rdx)  */

        /* Keep ring3 live long enough to exercise timer entry under shadow. */
        0x41, 0xbd, 0x00, 0xe1, 0xf5, 0x05,             /* mov $100000000,%r13d */
        0x41, 0xff, 0xcd, 0x75, 0xfb,                   /* dec r13d; jnz */
        0x41, 0xbc, 0x64, 0x00, 0x00, 0x00,             /* mov $100,%r12d */
        0x0f, 0x31,                                     /* rdtsc          */
        0x89, 0xc6,                                     /* mov %eax,%esi  */
        /* loop: */
        0xb8, 0x00, 0x00, 0x00, 0x00,                   /* mov $0,%eax (NOP) */
        0x0f, 0x05,                                     /* syscall        */
        0x41, 0xff, 0xcc,                               /* dec %r12d      */
        0x75, 0xf4,                                     /* jnz loop       */
        0x0f, 0x31,                                     /* rdtsc          */
        0x29, 0xf0,                                     /* sub %esi,%eax  */
        0x48, 0xba, 0x90, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x89, 0x42, 0x04,                               /* mov %eax,0x4(%rdx) */

        0xb8, 0x02, 0x00, 0x00, 0x00,                   /* mov $2,%eax (write) */
        0xbf, 0x01, 0x00, 0x00, 0x00,                   /* mov $1,%edi    */
        0x48, 0xbe, 0x80, 0x00, 0x40, 0x00, 0x00, 0x00,
        0x00, 0x00,                                     /* movabs $0x400080,%rsi */
        0xba, 0x05, 0x00, 0x00, 0x00,                   /* mov $5,%edx    */
        0x0f, 0x05,                                     /* syscall        */
        0xb8, 0x01, 0x00, 0x00, 0x00,                   /* mov $1,%eax (exit) */
        0x31, 0xff,                                     /* xor %edi,%edi  */
        0x0f, 0x05                                      /* syscall        */
    };
    memcpy(code, blob, sizeof(blob));
    memcpy((uint8_t *)code + 0x80, "hello", 6);

    uint64_t h = cm_hhdm();
    uint64_t code_pa = h ? (uint64_t)(uintptr_t)code - h : (uint64_t)(uintptr_t)code;
    uint64_t stk_pa  = h ? (uint64_t)(uintptr_t)stkp - h : (uint64_t)(uintptr_t)stkp;
    if (cm_space_map(pml4, USER_BASE, code_pa, 1) != 0 ||
        cm_space_map(pml4, USER_BASE + 0x1000, stk_pa, 1) != 0) {
        kerr("usermode: map user pages failed");
        return -1;
    }

    uint64_t before = cm_user_syscalls;
    uint64_t home = cm_paging_cr3();
    uint64_t shadow_cr3 = 0;
    uint64_t *shadow = cm_space_shadow(pml4, &shadow_cr3);
    if (!shadow) return -1;
#if CM_KPTI
    if (cm_space_lookup(shadow, (uint64_t)(uintptr_t)code) ||
        cm_space_lookup(shadow, (uint64_t)(uintptr_t)cm_land_user) ||
        !(cm_space_lookup(shadow, USER_BASE) & 4)) {
        kerr("KPTI: shadow isolation check failed");
        return -1;
    }
    kinfo("KPTI: shadow excludes heap and kernel C text; user mapping present");
#endif

    uint64_t user_rsp = USER_BASE + 0x2000 - 16;
    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    /* iretq pushes 5 and pops 5 — rsp is back at kstack. */
    cm_kern_rsp = kstack;
    int cpu = cm_cpu_id();
    cm_pcpu_init(cpu, kstack);
    g_pcpu[cpu].full_cr3 = cr3;
    g_pcpu[cpu].user_cr3 = shadow_cr3;
    uint64_t entry_top = (uint64_t)(uintptr_t)&g_entry_stacks[cpu][16384];
    cm_tss_set_rsp0(cpu, entry_top);
    g_pcpu[cpu].leave_user = 0;
    g_pcpu[cpu].entry_top=entry_top;

    kinfo("usermode: iretq → 0x%llx cs=0x2b ss=0x23 cr3=%llx",
          (unsigned long)USER_BASE, (unsigned long)cr3);

    g_home_cr3 = home;
    g_sys_before = before;

    cm_space_load(cr3); /* full process view supplies user buffers to C syscalls */
    cm_enter_user(shadow_cr3, USER_BASE, user_rsp, entry_top);
    return -1; /* not reached after a successful iretq */
}

void cm_usermode_run_elf(struct cm_proc *p)
{
    if (!g_um_ready[cm_cpu_id()]) cm_usermode_init();
    cm_thread_t *th = cm_sched_current();
    if (!p || !p->elf || !th || th->task != p->task || cm_cpu_id() != p->elf_cpu)
        cm_panic("exec: invalid ELF runner context");
    uint64_t irq = cm_irq_save();
    CM_UNUSED(irq); /* returns through scheduler rather than this frame */
    int cpu = cm_cpu_id();
    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    cm_pcpu_init(cpu, kstack);
    g_pcpu[cpu].full_cr3 = p->elf->cr3;
    g_pcpu[cpu].user_cr3 = p->elf->shadow_cr3;
    g_pcpu[cpu].incoming_cr3 = 0; g_pcpu[cpu].bad_entries = 0;
    uint64_t entry_top = (uint64_t)(uintptr_t)&g_entry_stacks[CM_PCPU_MAX+p->elf_entry_slot][16384];
    cm_tss_set_rsp0(cpu, entry_top);
    g_pcpu[cpu].leave_user = 0;
    g_pcpu[cpu].entry_top=entry_top;
    extern uint64_t cm_entry_user_irqs;
    if(!p->elf_started) {
        p->elf_timer_before = cm_entry_user_irqs;
        p->elf_start_tsc=cm_rdtsc();
    }
    p->elf_started = 1;
    kinfo("exec ring3: pid=%llu entry=%llx rsp=%llx cpu=%d",
          (unsigned long)p->pid, (unsigned long)(p->linux_resume?p->linux_regs.rcx:p->elf->entry),
          (unsigned long)(p->linux_resume?p->linux_regs.rsp:p->elf->stack), cpu);
    cm_arch_ctx_set_cr3(th->ctx_slot, p->elf->cr3);
    cm_space_load(p->elf->cr3);
    if(p->linux_resume) {
        extern void cm_enter_user_regs(uint64_t,cm_linux_regs_t *,uint64_t);
        cm_arch_user_fs_set(p->linux_fs);
        cm_enter_user_regs(p->elf->shadow_cr3,&p->linux_regs,entry_top);
    }
    cm_enter_user(p->elf->shadow_cr3, p->elf->entry, p->elf->stack, entry_top);
    cm_panic("exec: unexpected return from IRET");
}

void cm_land_user(void)
{
    cm_proc_t *elf_proc = cm_proc_current();
    if (elf_proc && elf_proc->elf) {
        int cpu = cm_cpu_id();
        uint64_t actual;
        __asm__ volatile("movq %%cr3, %0" : "=r"(actual));
        if ((!elf_proc->exited && !elf_proc->linux_thread_dead) || actual != elf_proc->elf->cr3)
            cm_panic("exec: exit did not retain full image CR3");
#if CM_KPTI
        if (g_pcpu[cpu].bad_entries || g_pcpu[cpu].incoming_cr3 != elf_proc->elf->shadow_cr3)
            cm_panic("exec: shadow CR3 mismatch");
#else
        CM_UNUSED(cpu);
#endif
        extern uint64_t cm_entry_user_irqs;
        elf_proc->elf_end_tsc=cm_rdtsc();
        g_kpti_verified=CM_KPTI && !g_pcpu[cpu].bad_entries;
        elf_proc->elf_run_ticks = elf_proc->main->run_ticks;
        elf_proc->elf_timer_entries = cm_entry_user_irqs - elf_proc->elf_timer_before;
        kinfo("exec exit: pid=%llu code=%lld writes=%d bytes=%llu timer_entries=%llu",
              (unsigned long)elf_proc->pid, (long)elf_proc->exit_code,
              elf_proc->elf_write_calls, (unsigned long)elf_proc->elf_written_bytes,
              (unsigned long)elf_proc->elf_timer_entries);
        kinfo("elf preempt: pid=%llu preempts=%llu run_ticks=%llu cpu=%d",
              (unsigned long)elf_proc->pid, (unsigned long)elf_proc->elf_preempts,
              (unsigned long)elf_proc->elf_run_ticks, cpu);
        cm_space_load(cm_paging_cr3());
        cm_arch_ctx_swap(elf_proc->main->ctx_slot, -1);
        cm_panic("exec: exited ELF thread resumed");
    }
#if CM_KPTI
    int cpu = cm_cpu_id();
    uint64_t actual;
    __asm__ volatile("movq %%cr3, %0" : "=r"(actual));
    if (actual != g_pcpu[cpu].full_cr3 || g_pcpu[cpu].bad_entries ||
        g_pcpu[cpu].incoming_cr3 != g_pcpu[cpu].user_cr3)
        cm_panic("KPTI: CR3 entry/exit mismatch");
    extern uint64_t cm_entry_user_irqs;
    if (!cm_entry_user_irqs) cm_panic("KPTI: no ring3 timer entry exercised");
    g_kpti_verified=1;
    kinfo("KPTI: verified shadow -> full CR3 on all 102 syscall entries, ring3 timer entries=%llu",
          (unsigned long)cm_entry_user_irqs);
#endif
    cm_space_load(g_home_cr3);
    uint64_t real_cpl = 0xff;
    if (g_user_code)
        real_cpl = g_user_code[0x90];
    cm_last_cpl = real_cpl;
    if (real_cpl != 3)
        kerr("usermode: user code ran at CPL=%llu (not 3)", (unsigned long)real_cpl);
    else
        kinfo("usermode: CPL=3 reported by user code itself (hard proof)");

    {
        uint32_t delta = 0;
        memcpy(&delta, (const uint8_t *)g_user_code + 0x94, sizeof(delta));
        if (delta)
            kinfo("e2e ring3: SYSCALL round trip = %u cycles/call (100 iters, real syscall insn)",
                  (unsigned)(delta / 100));
        else
            kwarn("e2e ring3: no rdtsc delta (user code did not run?)");
    }
    if (cm_user_syscalls < g_sys_before + 2)
        kerr("usermode: ring3 never completed a syscall round-trip");
    else
        kinfo("usermode: syscalls=%llu (write+exit)",
              (unsigned long)(cm_user_syscalls - g_sys_before));
    (void)cm_space_switch_cost();
    cm_selftest_finish();
}

#endif

/* Only a committed Linux exec calls this, with interrupts masked at entry.
 * Entry window/KPTI policy and per-thread kernel continuation stay unchanged. */
void cm_usermode_replace_image(void)
{
    cm_proc_t *p=cm_proc_current();
#ifndef CAPMIX_HOSTED
    int cpu=cm_cpu_id();g_pcpu[cpu].full_cr3=p->elf->cr3;g_pcpu[cpu].user_cr3=p->elf->shadow_cr3;
    g_pcpu[cpu].user_rsp=p->elf->stack;
    cm_arch_ctx_set_cr3(p->main->ctx_slot,p->elf->cr3);cm_space_load(p->elf->cr3);
#else
    (void)p;
#endif
    cm_arch_user_fs_set(0);
}
int64_t cm_syscall_frame(uint32_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d,cm_linux_regs_t *frame)
{
    cm_proc_t *p=cm_proc_current();
    if(p && p->abi==CM_ABI_LINUX) {
        p->linux_frame=frame;memcpy(&p->linux_regs,frame,120);
#ifndef CAPMIX_HOSTED
        p->linux_regs.rsp=g_pcpu[cm_cpu_id()].user_rsp;
#endif
    }
    int64_t result=cm_syscall(nr,a,b,c,d);
    if(p)p->linux_frame=NULL;
    /* A kill handled during dispatch uses the normal exit path. */
    if(p && (p->exited || p->linux_thread_dead))
        __atomic_store_n(&p->linux_migrate,0,__ATOMIC_RELEASE);
#ifndef CAPMIX_HOSTED
    if(p && __atomic_load_n(&p->linux_migrate,__ATOMIC_ACQUIRE)==1) {
        /* Dispatch has unwound. Disable IRQs before saving the live FS and
         * entry frame; no resource-owning C continuation is transported. */
        (void)cm_irq_save();
        memcpy(&p->linux_regs,frame,120);
        p->linux_regs.rax=(uint64_t)result;
        p->linux_regs.rsp=g_pcpu[cm_cpu_id()].user_rsp;
        p->linux_fs=cm_arch_user_fs_get();
        __atomic_store_n(&p->linux_migrate,2,__ATOMIC_RELEASE);
        /* Mirror the normal exit's SWAPGS phase without changing KPTI entry. */
        __asm__ volatile("swapgs" ::: "memory");
        cm_space_load(cm_paging_cr3());
        cm_arch_ctx_swap(p->main->ctx_slot,-1);
        cm_panic("linux migrate: discarded syscall tail resumed");
    }
#endif
    return result;
}
