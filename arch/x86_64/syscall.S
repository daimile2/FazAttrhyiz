#include "capmix/config.h"
	.globl cm_syscall_entry
	.type  cm_syscall_entry, @function
	.globl cm_user_rsp, cm_kern_rsp, cm_user_syscalls, cm_last_cpl
	.globl cm_leave_user, cm_land_user

	.section .entry.text,"ax",@progbits
cm_syscall_entry:
	swapgs
#if CM_KPTI
	movq %rax, %gs:40
	movq %cr3, %rax
	movq %rax, %gs:48
	cmpq %gs:32, %rax
	je 9f
	incq %gs:56
9:
	movq %gs:24, %rax
	movq %rax, %cr3
	movq %gs:40, %rax
#endif
	movq	%rsp, %gs:8
	movq	%gs:0, %rsp
	pushq %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	pushq	%r11
	pushq	%rcx
	pushq	%r9
	pushq	%r8
	pushq	%r10
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%rax
	lock incq	cm_user_syscalls(%rip)
	/* syscall ABI: rax=nr rdi=a0 rsi=a1 rdx=a2 r10=a3
	 * C ABI:       rdi=nr rsi=a0 rdx=a1 rcx=a2 r8=a3 */
	movq	0(%rsp), %rdi
	movq	8(%rsp), %rsi
	movq	16(%rsp), %rdx
	movq	24(%rsp), %rcx
	movq	32(%rsp), %r8
	movq %rsp, %r9 /* sixth argument: complete saved user frame */
	subq $8, %rsp /* SysV call alignment */
	call cm_syscall_frame
	addq $8, %rsp
	cmpq	$0, %gs:64
	jne	1f
	addq	$8, %rsp
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%r10
	popq	%r8
	popq	%r9
	popq	%rcx
	popq	%r11
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
#if CM_KPTI
	movq %rax, %gs:40
	movq %gs:32, %rax
	movq %rax, %cr3
	movq %gs:40, %rax
#endif
	movq	%gs:8, %rsp
	swapgs
	sysretq
1:
	movq	%gs:0, %rsp
	swapgs
	call	cm_land_user
2:
	hlt
	jmp	2b

/* SysV args: rdi=shadow CR3, rsi=user RIP, rdx=user RSP, rcx=entry top.
 * Everything after mov CR3, including the iret frame, lives in the window. */
	.globl cm_enter_user
cm_enter_user:
	cli
	movq %rcx, %rsp
	pushq $0x23
	pushq %rdx
	pushq $0x202
	pushq $0x2b
	pushq %rsi
	movq %rdi, %cr3
	iretq

	.data
	.align 8
cm_user_rsp:      .quad 0
	.globl cm_kern_rsp
cm_kern_rsp:      .quad 0
	.globl cm_user_syscalls
cm_user_syscalls: .quad 0
	.globl cm_last_cpl
cm_last_cpl:      .quad 0
	.globl cm_leave_user
cm_leave_user:    .quad 0

.section .note.GNU-stack,"",@progbits

/* Resume a fork child with independently saved registers (rax=0).
 * Input rdi=shadow CR3, rsi=register frame, rdx=entry stack top. */
.section .entry.text,"ax",@progbits
.globl cm_enter_user_regs
cm_enter_user_regs:
    cli
    movq %rdx,%rsp
    pushq $0x23
    pushq 120(%rsi)
    pushq 64(%rsi)
    pushq $0x2b
    pushq 56(%rsi)
    /* Copy registers into the shared entry window before loading shadow CR3. */
    movq %rsi,%r10
    movq $15,%rcx
1:  pushq (%r10)
    addq $8,%r10
    loop 1b
    movq %rdi,%cr3
    popq %rbp
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    popq %r11
    popq %rcx
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax
    iretq
