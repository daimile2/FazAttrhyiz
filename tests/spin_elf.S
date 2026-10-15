.section .text
.globl _start
_start:
    mov $2,%rax
    mov $1,%rdi
    lea message(%rip),%rsi
    mov $(message_end-message),%rdx
    xor %r10,%r10
    syscall
1:  pause
    jmp 1b
.section .rodata
message: .ascii "spin ELF: infinite ring3 loop ready\n"
message_end:
.section .note.GNU-stack,"",@progbits
