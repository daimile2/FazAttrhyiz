#ifndef CAPMIX_ARCH_H
#define CAPMIX_ARCH_H

#include "types.h"



void     cm_arch_early_init(void);
void     cm_serial_init(void);
void     cm_serial_putchar(char c);
uint64_t cm_rdtsc(void);
uint64_t cm_ticks_now(void);

void     cm_gdt_load(void);
void     cm_gdt_load_cpu(int cpu);
void     cm_idt_load(void);
void     cm_idt_load_ap(int cpu);
void      cm_smp_start(void);
void      cm_smp_dump(void);
int       cm_smp_online(void);
uint64_t cm_idt_firings(void);

/* 4-level page tables. Hosted: build only. Bare metal: may load CR3. */
int      cm_paging_init(cm_paddr_t phys_base, uint64_t npages);
uint64_t cm_paging_tables(void);
void     cm_paging_dump(void);
void     *cm_mmio_map(uint64_t phys, uint64_t npages);

/* Cooperative register/stack switch. Hosted uses swapcontext. */
int      cm_arch_ctx_init(int tid, void (*entry)(void *), void *arg, void *stack, size_t stack_sz);
void     cm_arch_ctx_swap(int from_tid, int to_tid); /* -1 = scheduler host */
int      cm_arch_ctx_done(int tid);
void     cm_arch_ctx_set_cr3(int tid, uint64_t cr3);
void     cm_arch_ctx_clear(int tid);
int      cm_arch_ctx_ready(int tid);
void     cm_tss_set_rsp0(int cpu, uint64_t top);
void     cm_tss_reset_rsp0(int cpu);
/* Restore the existing shadow-mapped entry stack after ELF timer/syscall scheduling. */
void     cm_usermode_resume_entry(void);

/* Local interrupt save/disable + restore. Anything that reads "which CPU am
 * I on" and then acts on per-CPU state must run between these: with IF=1 a
 * tick can preempt the thread in between, and the scheduler may resume it
 * on a DIFFERENT CPU, so the cpu index it computed is stale. Hosted is a
 * ring-3 process (cli faults) with no ISR, so both are no-ops there. */
static inline uint64_t cm_irq_save(void)
{
#ifndef CAPMIX_HOSTED
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
#else
    return 0;
#endif
}

static inline void cm_irq_restore(uint64_t f)
{
#ifndef CAPMIX_HOSTED
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
#else
    (void)f;
#endif
}

int      cm_limine_memory(cm_paddr_t *base, uint64_t *npages, uint64_t *hhdm);
int      cm_limine_mp(uint64_t *cpu_count, uint32_t *bsp_lapic_id,
                      uint32_t *lapic_ids, int max_ids);
int      cm_limine_mp_start(uint64_t idx, void *entry);
uint64_t  cm_limine_rsdp(void);
int cm_limine_usable(unsigned index, uint64_t *base, uint64_t *length);
int cm_cpu_apic_id(int cpu, uint32_t *id);
int cm_kpti_active(void);
int       cm_ioapic_init(void);
void      cm_idt_set_gate(int vec, void *fn, uint8_t ist);
uint64_t cm_hhdm(void);
uint64_t cm_kernel_phys(void);
uint64_t cm_kernel_virt(void);
uint64_t cm_kernel_size(void);

uint64_t cm_read_cr4(void);

unsigned cm_paging_levels(void);
int cm_paging_level_folded(unsigned level);
uint64_t cm_space_lookup(uint64_t *root, uint64_t virt);
uint64_t *cm_paging_pml4(void);
uint64_t  cm_paging_cr3(void);
uint64_t *cm_space_create(uint64_t *out_phys);
/* Call after mapping the user low half; shared low subtrees thereafter. */
uint64_t *cm_space_shadow(uint64_t *full, uint64_t *out_phys);
int       cm_space_map(uint64_t *pml4, uint64_t virt, uint64_t phys, int user);
int       cm_space_map_flags(uint64_t *pml4, uint64_t virt, uint64_t phys, int uncached);
void      cm_space_load(uint64_t cr3);
void      cm_paging_enable_pge(void);
void     *cm_boot_stack_base(void);

void      cm_lapic_map_before_cr3(void);
void      cm_lapic_init(void);
void      cm_lapic_eoi(void);
void      cm_timer_isr(void);
void      cm_timer_arm(void);
void      cm_irq_enable(void);
uint64_t  cm_timer_ticks(void);
uint32_t  cm_lapic_ccr(void);
uint64_t  cm_tsc_deadline(void);
int cm_timer_calibration_selftest(void);
void      cm_timer_dump_early_ticks(void);
int       cm_need_resched(void);
void      cm_need_resched_clear(void);
void      cm_sched_maybe_preempt(void);

/* Preemption (called from the timer ISR). */
void      cm_sched_preempt_now(void);
int       cm_sched_in_thread(void);
uint64_t  cm_sched_preempts(void);

/* SMP discovery. cm_cpu_id() returns 0 before the LAPIC is mapped and
 * before cm_smp_probe() has run, which is correct: the BSP is index 0. */
uint32_t  cm_lapic_id(void);
int       cm_cpu_id(void);
uint64_t  cm_tsc_hz(void);
int       cm_cpu_count(void);
int cm_smp_buddy_test(void (*callback)(unsigned));
void      cm_smp_probe(void);
void      cm_smp_sched_enable(void);
void      cm_smp_sched_disable(void);
void      cm_smp_sched_stress(uint64_t rounds);
uint64_t  cm_timer_ticks_cpu(int cpu);
uint64_t  cm_sched_preempts_cpu(int cpu);
uint64_t  cm_sched_lock_contend(void);
int       cm_sched_runnable(void);

/* SMP stress helpers: drain the runqueue to a defined state, and drop a
 * parked CPU's claim on the thread it last ran. */
void      cm_sched_release_current(void);
int       cm_sched_drain(void);

/* Fault reporting. cm_fault_install() re-points the error-code vectors
 * (8/10/11/12/13/14/17) at stubs that report and halt; it uses sidt and
 * so needs nothing from idt.c. */
void      cm_fault_isr(uint64_t vec, uint64_t err, uint64_t rip, uint64_t frame);
void      cm_fault_install(void);
void      cm_fault_dump_gate(uint32_t vec);

void     cm_usermode_init(void);
int      cm_usermode_selftest(void);
uint64_t cm_usermode_syscalls(void);
uint64_t cm_usermode_cpl(void);
struct cm_proc;
void cm_usermode_run_elf(struct cm_proc *proc);
uint64_t cm_space_switch_cost(void);

#endif