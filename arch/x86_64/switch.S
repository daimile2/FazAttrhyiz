#include "entry_asm.h"
/* Callee-saved + rsp/rip/rflags fiber switch. Works hosted and freestanding. */
.globl cm_ctx_switch_asm
.type  cm_ctx_switch_asm, @function
cm_ctx_switch_asm:
/* rdi = from, rsi = to
* layout: rbx rbp r12 r13 r14 r15 rsp rip rflags
*
* rflags is part of the context because a thread that was PREEMPTED must
* resume with IF=0 (it is still logically inside cm_timer_isr and gets IF=1
* back from its own iretq), while a fresh or cooperatively yielded thread
* must resume with IF=1. Deriving IF from whatever the CPU happened to be
* running with forces the caller to leave interrupts on across the swap,
* which is exactly the window that corrupted the scheduler stack. */
movq	%rbx,  0(%rdi)
movq	%rbp,  8(%rdi)
movq	%r12, 16(%rdi)
movq	%r13, 24(%rdi)
movq	%r14, 32(%rdi)
movq	%r15, 40(%rdi)
movq	%rsp, 48(%rdi)
leaq	1f(%rip), %rax
movq	%rax, 56(%rdi)
pushfq
popq	%rax
movq	%rax, 64(%rdi)

/* Restore. rsp is switched LAST but one, so popfq runs on the INCOMING
* stack: a tick landing between popfq and jmp re-saves the incoming
* context at exactly the rsp we just loaded, which is a no-op instead of
* "the scheduler's stack position recorded as the thread's context". */
movq	 0(%rsi), %rbx
movq	 8(%rsi), %rbp
movq	16(%rsi), %r12
movq	24(%rsi), %r13
movq	32(%rsi), %r14
movq	40(%rsi), %r15
movq	56(%rsi), %rax
movq	64(%rsi), %r11
movq	48(%rsi), %rsp
pushq	%r11
popfq
jmp	*%rax
1:
ret

.section .entry.text,"ax",@progbits
.globl cm_isr_stub
.type  cm_isr_stub, @function
cm_isr_stub:
CM_ENTRY_SAVE
pushq	%rax
lock incq cm_isr_count(%rip)
popq	%rax
CM_ENTRY_RESTORE
iretq

/* #DF/#TS/#NP/#SS/#GP/#PF/#AC push a hardware error code BELOW the
* standard 5-word frame. A stub that iretq's without accounting for it
* reads the error code as RIP and jumps to garbage; with IST=1 that
* becomes an endless fault->bad-iret->fault loop that never triple
* faults and therefore never prints anything. That is exactly the
* "silent hang until timeout" we have been chasing.
*
* These stubs never iretq. They hand the raw frame pointer to C (captured
* before alignment so C can read CS/RFLAGS/RSP/SS too), report, and halt. */
.macro FAULT_STUB vec
.globl cm_fault_stub_\vec
.type  cm_fault_stub_\vec, @function
cm_fault_stub_\vec:
CM_ENTRY_FAULT
movq	$\vec, %rdi	/* arg0: vector */
movq	0(%rsp), %rsi	/* arg1: error code */
movq	8(%rsp), %rdx	/* arg2: faulting RIP */
movq	%rsp, %rcx	/* arg3: frame base (points at error code) */
andq	$-16, %rsp	/* ABI alignment; we never return through here */
call	cm_fault_isr
1:	hlt
jmp	1b
.endm
FAULT_STUB 8	/* #DF */
FAULT_STUB 10	/* #TS */
FAULT_STUB 11	/* #NP */
FAULT_STUB 12	/* #SS */
FAULT_STUB 13	/* #GP */
/* #PF below has a recoverable L2 COW path. */
FAULT_STUB 17	/* #AC */

.globl cm_timer_stub
.type  cm_timer_stub, @function
cm_timer_stub:
CM_ENTRY_SAVE
#if CM_KPTI && !defined(CAPMIX_HOSTED)
/* Saved RAX/CR3 precede the hardware frame; CS is at +24. */
testb $3, 24(%rsp)
jz 990f
lock incq cm_entry_user_irqs(%rip)
990:
#endif
/* Full C-call path. Entry rsp is 8 (mod 16) after the hardware pushes
* 5 words; subq $8 then 10 pushes (80 bytes, 80%16==0) leaves rsp 0
* (mod 16) at the call site, which is what the SysV ABI wants. */
subq	$8, %rsp
pushq	%rax
pushq	%rcx
pushq	%rdx
pushq	%rsi
pushq	%rdi
pushq	%r8
pushq	%r9
pushq	%r10
pushq	%r11
pushq	%rbp
lock incq cm_isr_count(%rip)
call	cm_timer_isr
popq	%rbp
popq	%r11
popq	%r10
popq	%r9
popq	%r8
popq	%rdi
popq	%rsi
popq	%rdx
popq	%rcx
popq	%rax
addq	$8, %rsp
CM_ENTRY_RESTORE
iretq

.data
.globl cm_isr_count
cm_isr_count:
.quad	0

.section .note.GNU-stack,"",@progbits

.section .entry.text,"ax",@progbits
.globl cm_fault_stub_14
cm_fault_stub_14:
    CM_ENTRY_SAVE
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp,%r12
#if CM_KPTI && !defined(CAPMIX_HOSTED)
    leaq 136(%r12),%rcx
#else
    leaq 120(%r12),%rcx
#endif
    movq (%rcx),%rdi
    movq %cr2,%rsi
    movq 16(%rcx),%rdx
    andq $-16,%rsp
    call cm_linux_cow_fault
    testl %eax,%eax
    jz 2f
    movq %r12,%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    CM_ENTRY_RESTORE
    addq $8,%rsp /* hardware #PF error code */
    iretq
2:
#if CM_KPTI && !defined(CAPMIX_HOSTED)
    leaq 136(%r12),%rcx
#else
    leaq 120(%r12),%rcx
#endif
    movq $14,%rdi
    movq (%rcx),%rsi
    movq 8(%rcx),%rdx
    call cm_fault_isr
    ud2
