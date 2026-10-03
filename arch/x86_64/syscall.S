	.globl cm_syscall_entry
	.type  cm_syscall_entry, @function
	.globl cm_user_rsp, cm_kern_rsp, cm_user_syscalls, cm_last_cpl
	.globl cm_leave_user, cm_land_user

	.text
cm_syscall_entry:
	swapgs
	movq	%rsp, %gs:8
	movq	%gs:0, %rsp
	pushq	%r11
	pushq	%rcx
	pushq	%r9
	pushq	%r8
	pushq	%r10
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%rax
	incq	cm_user_syscalls(%rip)
	/* syscall ABI: rax=nr rdi=a0 rsi=a1 rdx=a2 r10=a3
	 * C ABI:       rdi=nr rsi=a0 rdx=a1 rcx=a2 r8=a3 */
	movq	0(%rsp), %rdi
	movq	8(%rsp), %rsi
	movq	16(%rsp), %rdx
	movq	24(%rsp), %rcx
	movq	32(%rsp), %r8
	call	cm_syscall
	cmpq	$0, cm_leave_user(%rip)
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
