/* Dedicated BSP driver wait ABI: 24(hid, writable message, 0, 0). */
.section .text
.global _start
_start:
    movw %cs,%ax
    andl $3,%eax
    movq %rax,reported_cpl(%rip)
    /* Retain the old polling receive's user-buffer and handle checks. */
    xorl %edx,%edx
    xorl %r10d,%r10d
    movl $24,%eax
    movl receive_handle(%rip),%edi
    movabs $0xffffffff80000000,%rsi
    syscall
    cmpq $-2,%rax
    jne failed
    movl $24,%eax
    movl receive_handle(%rip),%edi
    leaq _start(%rip),%rsi
    syscall
    cmpq $-2,%rax
    jne failed
    movl $7,%eax
    movl $12345,%edi
    leaq message(%rip),%rsi
    syscall
    cmpq $-3,%rax
    jne failed
    movl $24,%eax
    movl receive_handle(%rip),%edi
    leaq message(%rip),%rsi
    xorl %edx,%edx
    xorl %r10d,%r10d
    syscall
    movq %rax,receive_result(%rip)
    cmpl $1,expected_action(%rip)
    je cancelled
    cmpl $2,expected_action(%rip)
    je closed
    testq %rax,%rax
    jne failed
    cmpl $0x495251,message+8(%rip)
    jne failed
    cmpw $24,message+12(%rip)
    jne failed
    cmpw $0,message+14(%rip)
    jne failed
    cmpl $12,message+16(%rip)
    jne failed
    movl expected_driver(%rip),%eax
    cmpl %eax,message+20(%rip)
    jne failed
    cmpq $1,message+24(%rip)
    jne failed
    cmpq $1,message+32(%rip)
    jne failed
    incq received_count(%rip)
    /* Empty polling recv remains nonblocking, even with a wait-capable driver. */
    movl $7,%eax
    movl receive_handle(%rip),%edi
    leaq message(%rip),%rsi
    syscall
    cmpq $-5,%rax
    jne failed
    movl $2,%eax
    movl $1,%edi
    leaq hello(%rip),%rsi
    movl $19,%edx
    syscall
    cmpq $19,%rax
    jne failed
    jmp done
cancelled:
    cmpq $-5,%rax
    jne failed
    jmp done
closed:
    cmpq $-9,%rax
    jne failed
done:
    movl $1,%eax
    xorl %edi,%edi
    syscall
    ud2
failed:
    movl $1,%eax
    movl $1,%edi
    syscall
    ud2
hello: .ascii "driver irq wake ok\n\0"
.section .data
receive_handle: .long 0
expected_driver: .long 0
expected_action: .long 0
reserved: .long 0
received_count: .quad 0
reported_cpl: .quad 0
receive_result: .quad 0x777
.section .bss
.balign 16
message: .skip 96
.section .note.GNU-stack,"",@progbits
