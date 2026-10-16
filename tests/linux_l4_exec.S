.global _start
.section .text
_start:
    mov $59,%eax
    lea path(%rip),%rdi
    lea args(%rip),%rsi
    xor %edx,%edx
    syscall
    /* Failed preparation must leave this old image and its data runnable. */
    cmp $-2,%rax
    jne fail
    mov $1,%eax
    mov $1,%edi
    lea message(%rip),%rsi
    mov $message_end-message,%edx
    syscall
    mov $60,%eax
    mov $42,%edi
    syscall
fail:
    mov $60,%eax
    mov $93,%edi
    syscall
    ud2
.section .data
path: .asciz "/bin/linux-l4a.elf"
arg: .asciz "exec"
.align 8
args: .quad path,arg,0
message: .ascii "linux L4a exec failure: old image survived\n"
message_end:
.section .note.GNU-stack,"",@progbits
