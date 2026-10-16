.global _start
.hidden rtld_main
.section .text
_start:
    mov %rsp,%r12
    mov %rsp,%rdi
    and $-16,%rsp
    call rtld_main
    mov %r12,%rsp
    xor %edx,%edx
    jmp *%rax
.section .note.GNU-stack,"",@progbits
