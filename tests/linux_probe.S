/* Linux-only auxv, errno and FS-base/preemption isolation probe. */
.section .text
.global _start
_start:
    movq (%rsp),%rax
    cmpq $2,%rax
    jne fail
    movq 16(%rsp),%rax
    leaq tls_a(%rip),%r12
    cmpb $65,(%rax)
    je 1f
    leaq tls_b(%rip),%r12
1:
    leaq 16(%rsp),%rsi
    movq (%rsp),%rax
    leaq (%rsi,%rax,8),%rsi
2:  lodsq
    testq %rax,%rax
    jnz 2b
    xorl %r13d,%r13d
3:  lodsq
    movq (%rsi),%rdx
    addq $8,%rsi
    testq %rax,%rax
    jz aux_done
    cmpq $6,%rax
    jne 4f
    cmpq $4096,%rdx
    jne fail
    orl $1,%r13d
4:  cmpq $3,%rax
    jne 5f
    cmpq $0x400040,%rdx
    jne fail
    cmpl $1,(%rdx)
    jne fail
    orl $2,%r13d
5:  cmpq $4,%rax
    jne 6f
    cmpq $56,%rdx
    jne fail
    orl $4,%r13d
6:  cmpq $5,%rax
    jne 7f
    cmpq $2,%rdx
    jne fail
    orl $8,%r13d
7:  cmpq $9,%rax
    jne 8f
    leaq _start(%rip),%rcx
    cmpq %rcx,%rdx
    jne fail
    orl $16,%r13d
8:  cmpq $25,%rax
    jne 3b
    cmpq $0x7fc000,%rdx
    jb fail
    cmpq $0x7ffff0,%rdx
    ja fail
    movq (%rdx),%rax
    movq 8(%rdx),%rdx
    orl $32,%r13d
    jmp 3b
aux_done:
    cmpq $63,%r13
    jne fail
    testq %rdx,%rdx
    jnz fail
    movl $158,%eax
    movl $0x1003,%edi
    leaq scratch(%rip),%rsi
    syscall
    testq %rax,%rax
    jnz fail
    cmpq $0,scratch(%rip)
    jne fail
    movl $9999,%eax
    syscall
    cmpq $-38,%rax
    jne fail
    movl $0x40000000,%eax /* unsupported x32 ABI must not enter native dispatch */
    syscall
    cmpq $-38,%rax
    jne fail
    movl $1,%eax
    movl $100,%edi
    xorl %esi,%esi
    movl $1,%edx
    syscall
    cmpq $-9,%rax
    jne fail
    movl $1,%eax
    movl $1,%edi
    syscall
    cmpq $-14,%rax
    jne fail
    movl $158,%eax
    movl $0x1002,%edi
    movabsq $0xffff800000000000,%rsi
    syscall
    cmpq $-1,%rax
    jne fail
    movl $158,%eax
    movl $0x1003,%edi
    movl $0x400000,%esi
    syscall
    cmpq $-14,%rax
    jne fail
    movl $158,%eax
    movl $0x1002,%edi
    movq %r12,%rsi
    syscall
    testq %rax,%rax
    jne fail
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    addq budget(%rip),%rax
    movq %rax,%r14
spin:
    movq %fs:0,%rax
    cmpq (%r12),%rax
    jne fail
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    cmpq %r14,%rax
    jb spin
    movl $158,%eax
    movl $0x1003,%edi
    leaq scratch(%rip),%rsi
    syscall
    testq %rax,%rax
    jne fail
    cmpq %r12,scratch(%rip)
    jne fail
    movq $1,report(%rip)
    movl $1,%eax
    movl $1,%edi
    leaq message(%rip),%rsi
    movl $message_end-message,%edx
    syscall
    cmpq $message_end-message,%rax
    jne fail
    movl $60,%eax
    xorl %edi,%edi
    syscall
    ud2
fail:
    movl $60,%eax
    movl $42,%edi
    syscall
    ud2
.section .rodata
message: .ascii "linux auxv/fs isolation ok\n"
message_end:
.section .data
report: .quad 0
budget: .quad 100000000
scratch: .quad 0
.org 0x100
tls_a: .quad 0x1122334455667788
.org 0x200
tls_b: .quad 0x8877665544332211
.section .note.GNU-stack,"",@progbits
