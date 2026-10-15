.global _start
.section .text
_start:
    xor %ebp,%ebp
    mov (%rsp),%rdi
    lea 8(%rsp),%rsi
    and $-16,%rsp
    call main
    mov %eax,%edi
    mov $60,%eax
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
