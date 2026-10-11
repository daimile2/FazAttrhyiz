/* Native UDP capabilities: open=17 bind=18 send=19 recv=20 close=21. */
.section .text
.global _start
_start:
    movw %cs, %ax
    andl $3, %eax
    movq %rax, reported_cpl(%rip)
    movl $17, %eax
    syscall
    testq %rax, %rax
    jle failed
    movq %rax, %r12
    movl $18, %eax
    movq %r12, %rdi
    movl lo_ifid(%rip), %esi
    movl $9100, %edx
    syscall
    testq %rax, %rax
    jne failed
    /* Invalid send buffer, then read-only recv buffer: do not touch kernel/text. */
    movl $19, %eax
    movq %r12, %rdi
    movl $9100, %esi
    movabs $0xffffffff80000000, %rdx
    movl $17, %r10d
    syscall
    cmpq $-2, %rax
    jne failed
    movl $20, %eax
    movq %r12, %rdi
    leaq _start(%rip), %rsi
    syscall
    cmpq $-2, %rax
    jne failed
    movl $19, %eax
    movq %r12, %rdi
    movl $9100, %esi
    leaq payload(%rip), %rdx
    movl $17, %r10d
    syscall
    cmpq $17, %rax
    jne failed
    movl $20, %eax
    movq %r12, %rdi
    leaq datagram(%rip), %rsi
    syscall
    cmpq $17, %rax
    jne failed
    movl %eax, receive_len(%rip)
    cmpl $0x7f000001, datagram(%rip)
    jne failed
    cmpl $0x7f000001, datagram+4(%rip)
    jne failed
    cmpw $9100, datagram+8(%rip)
    jne failed
    cmpw $9100, datagram+10(%rip)
    jne failed
    cmpw $17, datagram+12(%rip)
    jne failed
    leaq payload(%rip), %rsi
    leaq datagram+16(%rip), %rdi
    movl $17, %ecx
    repe cmpsb
    jne failed
    movl $20, %eax
    movq %r12, %rdi
    leaq datagram(%rip), %rsi
    syscall
    cmpq $-5, %rax
    jne failed
    movl $21, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    jne failed
    /* Leave a second bound endpoint for EXIT cleanup to reclaim. */
    movl $17, %eax
    syscall
    testq %rax, %rax
    jle failed
    movq %rax, %rdi
    movl $18, %eax
    movl lo_ifid(%rip), %esi
    movl $9101, %edx
    syscall
    testq %rax, %rax
    jne failed
    movl $2, %eax
    movl $1, %edi
    leaq hello(%rip), %rsi
    movl $13, %edx
    syscall
    cmpq $13, %rax
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
lo_ifid: .long 0
receive_len: .long 0
reported_cpl: .quad 0
payload: .ascii "udp-loopback-test"
hello: .ascii "udp lo rx ok\n"
.section .bss
.balign 16
datagram: .skip 272
.section .note.GNU-stack,"",@progbits
