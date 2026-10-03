/* Callee-saved + rsp/rip fiber switch. Works hosted and freestanding. */
.globl cm_ctx_switch_asm
.type  cm_ctx_switch_asm, @function
cm_ctx_switch_asm:
/* rdi = from, rsi = to
* layout: rbx rbp r12 r13 r14 r15 rsp rip */
movq	%rbx,  0(%rdi)
movq	%rbp,  8(%rdi)
movq	%r12, 16(%rdi)
movq	%r13, 24(%rdi)
movq	%r14, 32(%rdi)
movq	%r15, 40(%rdi)
movq	%rsp, 48(%rdi)
leaq	1f(%rip), %rax
movq	%rax, 56(%rdi)
movq	 0(%rsi), %rbx
movq	 8(%rsi), %rbp
movq	16(%rsi), %r12
movq	24(%rsi), %r13
movq	32(%rsi), %r14
movq	40(%rsi), %r15
movq	48(%rsi), %rsp
movq	56(%rsi), %rax
jmp	*%rax
1:
ret

.globl cm_isr_stub
.type  cm_isr_stub, @function
cm_isr_stub:
pushq	%rax
incq	cm_isr_count(%rip)
popq	%rax
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
FAULT_STUB 14	/* #PF */
FAULT_STUB 17	/* #AC */

.globl cm_timer_stub
.type  cm_timer_stub, @function
cm_timer_stub:
/* Full C-call path. Entry rsp is 8 (mod 16) after the hardware pushes
* 5 words; subq $8 then 11 pushes (88 bytes, 88%16==8) leaves rsp 0
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
incq	cm_isr_count(%rip)
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
iretq

.data
.globl cm_isr_count
cm_isr_count:
.quad	0
