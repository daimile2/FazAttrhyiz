/* Fixed Linux x86-64 L0 fixture. No libc and no CapMix syscall numbers. */
.section .text
.global _start
_start:
    movl $1,%edi
    leaq message(%rip),%rsi
    movl $message_end-message,%edx
    movl $1,%eax
    syscall
    cmpq $message_end-message,%rax
    jne fail
    movl $60,%eax
    xorl %edi,%edi
    syscall
    ud2
fail:
    movl $60,%eax
    movl $1,%edi
    syscall
    ud2
.section .rodata
message: .ascii "hello from Linux ABI L0\n"
message_end:
.section .data
.quad 0
.section .note.GNU-stack,"",@progbits
