#include "capmix/arch.h"
#include "capmix/syscall.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/kernel.h"

#include <string.h>

extern void     cm_syscall_entry(void);
extern uint64_t cm_kern_rsp;
extern uint64_t cm_user_syscalls;
extern uint64_t cm_last_cpl;
extern uint64_t cm_leave_user;

#define USER_CS  0x2B
#define USER_SS  0x23

#define CM_PCPU_MAX 8
typedef struct { uint64_t rsp0; uint64_t user_rsp; uint64_t kern_rsp; } cm_pcpu_t;
static cm_pcpu_t g_pcpu[CM_PCPU_MAX] __attribute__((aligned(64)));

static inline void pcpu_wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

void cm_pcpu_init(int cpu, uint64_t kstack_top)
{
    if (cpu < 0 || cpu >= CM_PCPU_MAX) return;
    g_pcpu[cpu].rsp0     = kstack_top;
    g_pcpu[cpu].kern_rsp = kstack_top;
    pcpu_wrmsr(0xC0000102, (uint64_t)(uintptr_t)&g_pcpu[cpu]);
}

#define USER_BASE 0x400000ull

#ifdef CAPMIX_HOSTED

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

static int g_um_ready;
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
    wrmsr(0xC0000080, efer | 1ull);
    wrmsr(0xC0000081, (0x18ull << 48) | (0x08ull << 32));
    wrmsr(0xC0000082, (uint64_t)(uintptr_t)cm_syscall_entry);
    wrmsr(0xC0000084, 0x257fdull);
    g_um_ready = 1;
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
    if (!g_um_ready) cm_usermode_init();
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
    cm_space_load(cr3);

    uint64_t user_rsp = USER_BASE + 0x2000 - 16;
    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    /* iretq pushes 5 and pops 5 — rsp is back at kstack. */
    cm_kern_rsp = kstack;
    cm_pcpu_init(cm_cpu_id(), kstack);
    cm_leave_user = 0;

    kinfo("usermode: iretq → 0x%llx cs=0x2b ss=0x23 cr3=%llx",
          (unsigned long)USER_BASE, (unsigned long)cr3);

    g_home_cr3 = home;
    g_sys_before = before;

    __asm__ volatile(
        "pushq %[ss]\n\t"
        "pushq %[rsp]\n\t"
        "pushq $0x202\n\t"
        "pushq %[cs]\n\t"
        "pushq %[rip]\n\t"
        "iretq\n\t"
        :
        : [ss] "i"(USER_SS), [cs] "i"(USER_CS),
          [rsp] "r"(user_rsp), [rip] "r"(USER_BASE)
        : "memory"
    );
    return -1; /* not reached after a successful iretq */
}

void cm_land_user(void)
{
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
