/* CapMix IRQ runtime test. syscalls: IPC_RECV=7, WRITE=2, EXIT=1. */
.section .text
.global _start
_start:
    movw %cs, %ax
    andl $3, %eax
    movq %rax, reported_cpl(%rip)
    movl $7, %eax
    movl receive_handle(%rip), %edi
    movabs $0xffffffff80000000, %rsi
    syscall
    cmpq $-2, %rax
    jne failed
    movl $7, %eax
    movl receive_handle(%rip), %edi
    leaq _start(%rip), %rsi /* RX text is not a writable receive buffer */
    syscall
    cmpq $-2, %rax
    jne failed
    movl $7, %eax
    movl $12345, %edi
    leaq message(%rip), %rsi
    syscall
    cmpq $-3, %rax
    jne failed
    movl $7, %eax
    movl receive_handle(%rip), %edi
    leaq message(%rip), %rsi
    syscall
    testq %rax, %rax
    jne failed
    cmpl $0x495251, message+8(%rip)
    jne failed
    cmpw $24, message+12(%rip)
    jne failed
    cmpw $0, message+14(%rip)
    jne failed
    cmpl $12, message+16(%rip)
    jne failed
    movl expected_driver(%rip), %eax
    cmpl %eax, message+20(%rip)
    jne failed
    cmpq $1, message+24(%rip)
    jne failed
    cmpq $1, message+32(%rip)
    jne failed
    incq received_count(%rip)
    /* No pending message: AGAIN must not park this serial ELF task. */
    movl $7, %eax
    movl receive_handle(%rip), %edi
    leaq message(%rip), %rsi
    syscall
    cmpq $-5, %rax
    jne failed
    movl $2, %eax
    movl $1, %edi
    leaq hello(%rip), %rsi
    movl $16, %edx
    syscall
    cmpq $16, %rax
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
receive_handle: .long 0
expected_driver: .long 0
received_count: .quad 0
reported_cpl: .quad 0
hello: .ascii "driver irq recv\n"
.section .bss
.balign 16
message: .skip 96
.section .note.GNU-stack,"",@progbits
