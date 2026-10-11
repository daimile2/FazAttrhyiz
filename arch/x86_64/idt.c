#include "capmix/arch.h"
#include "capmix/klog.h"
#include "capmix/arch.h"
#include "capmix/config.h"      /* ← 新增：CM_MAX_CPUS 的唯一来源 */
#include "capmix/klog.h"
#include <string.h>

struct idt_gate {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  type;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss64 {
    uint32_t res0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t res1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t res2;
    uint16_t res3, iopb;
} __attribute__((packed));

extern void     cm_isr_stub(void);
extern void     cm_timer_stub(void);
extern uint64_t cm_isr_count;
extern void     cm_fault_stub_8(void);
extern void     cm_fault_stub_10(void);
extern void     cm_fault_stub_11(void);
extern void     cm_fault_stub_12(void);
extern void     cm_fault_stub_13(void);
extern void     cm_fault_stub_14(void);
extern void     cm_fault_stub_17(void);

/* The IDT is SHARED by all CPUs (same gates, same handlers). The GDT is
 * NOT: entry 6/7 is the TSS descriptor, and each CPU needs its own TSS to
 * get its own RSP0 and its own IST stacks. Sharing one TSS across CPUs
 * means two CPUs can take a #PF at the same instant and land on the same
 * IST stack, which corrupts both. */
static struct idt_gate g_idt[256] __attribute__((section(".entry.data"), aligned(4096)));
static struct tss64 g_tss[CM_MAX_CPUS] __attribute__((section(".entry.data")));
static uint8_t         g_ist1[CM_MAX_CPUS][16384] __attribute__((section(".entry.data"), aligned(4096)));
static uint8_t         g_ist2[CM_MAX_CPUS][16384] __attribute__((section(".entry.data"), aligned(4096)));
static uint64_t g_gdt[CM_MAX_CPUS][10] __attribute__((section(".entry.data")));

static void set_gate(int vec, void *fn, uint8_t ist)
{
    uint64_t off = (uint64_t)(uintptr_t)fn;
    g_idt[vec].off_lo  = (uint16_t)off;
    g_idt[vec].sel     = 0x08;
    g_idt[vec].ist     = ist & 7;
    g_idt[vec].type    = 0x8E;
    g_idt[vec].off_mid = (uint16_t)(off >> 16);
    g_idt[vec].off_hi  = (uint32_t)(off >> 32);
    g_idt[vec].zero    = 0;
}

static void idt_build(void)
{
    memset(g_idt, 0, sizeof(g_idt));
#ifndef CAPMIX_HOSTED
    for (int i = 0; i < 32; i++)
        set_gate(i, (void *)cm_isr_stub, 0);
    /* Every vector the CPU pushes an error code for must use a FAULT_STUB,
     * not cm_isr_stub: cm_isr_stub iretq's without popping the error code,
     * so it reads the code as RIP and jumps into garbage. On an IST stack
     * that becomes fault -> bad iret -> fault with no triple fault, i.e.
     * the silent hang we keep chasing. #TS/#NP/#AC used to be installed
     * here as plain cm_isr_stub. */
    set_gate(8,  (void *)cm_fault_stub_8,  1); /* #DF */
    set_gate(10, (void *)cm_fault_stub_10, 1); /* #TS */
    set_gate(11, (void *)cm_fault_stub_11, 1); /* #NP */
    set_gate(12, (void *)cm_fault_stub_12, 1); /* #SS */
    set_gate(13, (void *)cm_fault_stub_13, 1); /* #GP */
    set_gate(14, (void *)cm_fault_stub_14, 1); /* #PF */
    set_gate(17, (void *)cm_fault_stub_17, 1); /* #AC */
    /* IST=0: current stack. Avoids IST tops that may sit past kernel map. */
    set_gate(0x40, (void *)cm_timer_stub, 0);
#endif
}

static void idt_load_table(void)
{
    struct idtr idtr = { .limit = sizeof(g_idt) - 1, .base = (uint64_t)(uintptr_t)g_idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));
}

void cm_idt_load(void)
{
    cm_gdt_load(); /* TSS + ltr must be live before any interrupt gate fires */
    idt_build();
#ifndef CAPMIX_HOSTED
    idt_load_table();
    kinfo("idt: lidt base=%p #PF/#GP/#DF/#SS IST=1 timer=0x40 IST=0", (void *)g_idt);
#else
    kinfo("idt: table built (%llu bytes), lidt skipped on hosted",
          (unsigned long)sizeof(g_idt));
#endif
}

/* AP path: build the CPU's OWN GDT/TSS, then point at the SHARED IDT.
 * idt_build() must NOT run again - the BSP already filled the table and a
 * second memset would race with any CPU already taking interrupts. */
void cm_idt_load_ap(int cpu)
{
    cm_gdt_load_cpu(cpu);
#ifndef CAPMIX_HOSTED
    idt_load_table();
#endif
}

void cm_gdt_load(void)
{
    cm_gdt_load_cpu(cm_cpu_id());
}

void cm_gdt_load_cpu(int cpu)
{
#ifndef CAPMIX_HOSTED
    if (cpu < 0 || cpu >= CM_MAX_CPUS) cpu = 0;

    memset(&g_tss[cpu], 0, sizeof(g_tss[cpu]));

        g_tss[cpu].ist1 = (uint64_t)(uintptr_t)&g_ist1[cpu][sizeof(g_ist1[cpu])];
        g_tss[cpu].ist2 = (uint64_t)(uintptr_t)&g_ist2[cpu][sizeof(g_ist2[cpu])];
        g_tss[cpu].rsp0 = (uint64_t)(uintptr_t)&g_ist2[cpu][sizeof(g_ist2[cpu])];
        g_tss[cpu].iopb = sizeof(g_tss[cpu]);
    uint64_t tb = (uint64_t)(uintptr_t)&g_tss[cpu];
    uint64_t lim = sizeof(g_tss[cpu]) - 1;

    memset(g_gdt[cpu], 0, sizeof(g_gdt[cpu]));
    g_gdt[cpu][1] = 0x00af9a000000ffffull; /* 0x08 kernel 64 code */
    g_gdt[cpu][2] = 0x00cf92000000ffffull; /* 0x10 kernel data */
    g_gdt[cpu][3] = 0x00cffa000000ffffull; /* 0x18 user 32 code */
    g_gdt[cpu][4] = 0x00cff2000000ffffull; /* 0x20 user data */
    g_gdt[cpu][5] = 0x00affa000000ffffull; /* 0x28 user 64 code */
    g_gdt[cpu][6] =
        (lim & 0xffff) |
        ((tb & 0xffffff) << 16) |
        (0x89ull << 40) |
        (((lim >> 16) & 0xf) << 48) |
        (((tb >> 24) & 0xff) << 56);
    g_gdt[cpu][7] = tb >> 32;

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr = { sizeof(g_gdt[cpu]) - 1, (uint64_t)(uintptr_t)g_gdt[cpu] };
    __asm__ volatile("lgdt %0" :: "m"(gdtr));
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)0x30));
    kinfo("gdt: lgdt + ltr 0x30 cpu=%d ist1=%p ist2=%p",
          cpu, (void *)(uintptr_t)g_tss[cpu].ist1, (void *)(uintptr_t)g_tss[cpu].ist2);

    /* lgdt does NOT reload segment registers. CS/DS/ES/SS still hold
     * Limine's selectors (cs=0x28 ss=0x30), which in OUR new GDT are
     * user-code DPL=3 and the TSS descriptor respectively. iretq then
     * pops CS=0x28 from the interrupt frame and faults #GP(err=0x28).
     * Fix: far-return to reload CS=0x08, then set DS/ES/SS=0x10. */
    {
        uint32_t cs_b = 0, ds_b = 0, ss_b = 0;
        __asm__ volatile("movl %%cs, %0" : "=r"(cs_b));
        __asm__ volatile("movl %%ds, %0" : "=r"(ds_b));
        __asm__ volatile("movl %%ss, %0" : "=r"(ss_b));

        __asm__ volatile(
            "pushq $0x08\n\t"          /* kernel 64 code, DPL=0 */
            "leaq 1f(%%rip), %%rax\n\t"
            "pushq %%rax\n\t"
            "lretq\n\t"                /* reload CS */
            "1:\n\t"
            "movw $0x10, %%ax\n\t"     /* kernel data, DPL=0 */
            "movw %%ax, %%ds\n\t"
            "movw %%ax, %%es\n\t"
            "movw %%ax, %%ss\n\t"
            "xorl %%eax, %%eax\n\t"
            "movw %%ax, %%fs\n\t"
            "movw %%ax, %%gs\n\t"
            ::: "rax", "memory");

        uint32_t cs_a = 0, ss_a = 0;
        __asm__ volatile("movl %%cs, %0" : "=r"(cs_a));
        __asm__ volatile("movl %%ss, %0" : "=r"(ss_a));
        kinfo("gdt: seg reload was cs=%x ds=%x ss=%x -> now cs=%x ss=%x",
              cs_b, ds_b, ss_b, cs_a, ss_a);
    }
#else
    (void)cpu;
    kinfo("gdt: skipped on hosted (host libc owns segments)");
#endif
}

uint64_t cm_idt_firings(void)
{
#ifndef CAPMIX_HOSTED
    return cm_isr_count;
#else
    return 0;
#endif
}

uint64_t cm_read_cr4(void)
{
#ifndef CAPMIX_HOSTED
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

/* Exported so ioapic.c can install its own vectors. The table is already
 * loaded by lidt, so filling g_idt[] here is enough. Appended at EOF: an
 * earlier attempt anchored after cm_idt_firings() landed inside a
 * conditional block and the bare-metal link could not see it. */
void cm_idt_set_gate(int vec, void *fn, uint8_t ist)
{
    if (vec < 0 || vec >= 256) return;
#ifndef CAPMIX_HOSTED
    set_gate(vec, fn, ist);
#else
    (void)fn;
    (void)ist;
#endif
}

/* Point this CPU's ring0 stack at whatever is about to run.
 *
 * rsp0 is initialised once to g_ist2[cpu] and NEVER changed, which makes
 * every thread on a CPU share one ring0 stack. A thread preempted while
 * running in ring3 leaves its iret frame at ist2_top-40 and its saved
 * registers around ist2_top-136; the next ring3 interrupt on that CPU
 * pushes a fresh frame at exactly the same addresses and overwrites both.
 * When the first thread is switched back it unwinds through the remains
 * and iretq pops the OTHER thread's RIP/CS/RFLAGS.
 *
 * Handing each thread its own kstack as rsp0 removes the sharing: an
 * abandoned frame sits on the stack of the thread that owns it, and
 * nothing else ever pushes there. cm_arch_ctx_swap() calls this on every
 * switch into a thread slot. */
void cm_tss_set_rsp0(int cpu, uint64_t top)
{
#ifndef CAPMIX_HOSTED
    if (cpu < 0 || cpu >= CM_MAX_CPUS) cpu = 0;
    g_tss[cpu].rsp0 = top;
#else
    (void)cpu;
    (void)top;
#endif
}

/* Back to the per-CPU default stack. Used when the CPU is not running a
 * thread (boot, AP idle, or switching into the scheduler context): the
 * g_ist2[] stack is otherwise unused once threads own their own ring0
 * stacks, so it makes a safe home for the scheduler. */
void cm_tss_reset_rsp0(int cpu)
{
#ifndef CAPMIX_HOSTED
    if (cpu < 0 || cpu >= CM_MAX_CPUS) cpu = 0;
    g_tss[cpu].rsp0 = (uint64_t)(uintptr_t)&g_ist2[cpu][16384];
#else
    (void)cpu;
#endif
}
