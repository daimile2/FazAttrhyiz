.global _start
.section .text
_start:
    xor %ebp,%ebp
    and $-16,%rsp
    call main
    mov %eax,%edi
    mov $60,%eax
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
