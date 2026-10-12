/* CapMix native ABI: write=2, exit=1, fork compat=128 (not Linux syscall ABI). */
.section .text
.global _start
_start:
    movw %cs, %ax
    andl $3, %eax
    movb %al, cpl_report(%rip)
    cmpq $0, bss_probe(%rip)
    jne failed
    /* Invalid stdout buffer must be rejected without reading kernel memory. */
    movl $2, %eax
    movl $1, %edi
    movabs $0xffffffff80000000, %rsi
    movl $1, %edx
    syscall
    cmpq $-2, %rax
    jne failed
    /* fork stays NOTSUP and must not dereference this invalid output pointer. */
    movl $128, %eax
    movabs $0xffffffff80000000, %rdi
    syscall
    cmpq $-7, %rax
    jne failed
    /* Observe IF and verify registers/stack survive actual timer scheduling. */
    pushfq
    popq %rax
    movq %rax, flags_report(%rip)
    testq $0x200,%rax
    jz failed
    movl $0x13579,%ebx
    movl $0x24680,%ebp
    movl $0x12345,%r13d
    movl $0x23456,%r14d
    movl $0x34567,%r15d
    movl $0x45678,%r8d
    movl $0x56789,%r9d
    movl $0x67890,%r10d
    pushq %rbx
    movl $100000000, %r12d
1:  decl %r12d
    jnz 1b
    popq %rax
    cmpq $0x13579,%rax
    jne failed
    cmpq $0x13579,%rbx
    jne failed
    cmpq $0x24680,%rbp
    jne failed
    cmpq $0x12345,%r13
    jne failed
    cmpq $0x23456,%r14
    jne failed
    cmpq $0x34567,%r15
    jne failed
    cmpq $0x45678,%r8
    jne failed
    cmpq $0x56789,%r9
    jne failed
    cmpq $0x67890,%r10
    jne failed
    movl $2, %eax
    movl $1, %edi
    leaq hello(%rip), %rsi
    movl $15, %edx
    syscall
    cmpq $15, %rax
    jne failed
    movl $1, %eax
    xorl %edi, %edi
    syscall
    ud2
failed:
    movl $1, %eax
    movl $1, %edi
    syscall
    ud2
.section .data
hello: .ascii "hello from elf\n"
.section .bss
.balign 64
cpl_report: .skip 1
.balign 8
bss_probe: .skip 8
flags_report: .skip 8
.section .note.GNU-stack,"",@progbits
