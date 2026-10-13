/* Fixed-address private data, register and stack canaries across real ticks. */
.section .text
.global _start
_start:
    movw %cs,%ax
    andl $3,%eax
    movq %rax,cpl(%rip)
    movq token(%rip),%r13
    pushq %r13
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    movq %rax,start(%rip)
    addq duration(%rip),%rax
    movq %rax,%r12
1:  incq heartbeat(%rip)
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    cmpq %r12,%rax
    jb 1b
    movq %rax,finish(%rip)
    popq %rax
    cmpq %r13,%rax
    jne failed
    cmpq token(%rip),%r13
    jne failed
    movl $0,%eax
    syscall
    testq %rax,%rax
    jne failed
    movl $2,%eax
    movl $1,%edi
    leaq msg(%rip),%rsi
    movl $21,%edx
    syscall
    cmpq $21,%rax
    jne failed
    movl $1,%eax
    xorl %edi,%edi
    syscall
    ud2
failed:
    movl $1,%eax
    movl $1,%edi
    syscall
    ud2
.section .data
duration: .quad 10000000000
 token: .quad 0x13579
 start: .quad 0
 finish: .quad 0
 heartbeat: .quad 0
 cpl: .quad 0
msg: .ascii "concurrent elf done!\n"
.section .note.GNU-stack,"",@progbits
