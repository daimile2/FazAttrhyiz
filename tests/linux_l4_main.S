.global _start
.section .interp,"a"
.asciz "/lib/ld-capmix-l4a.so"
.section .text
_start:
    mov %cs,%ax
    and $3,%eax
    cmp $3,%eax
    jne fail
    movq $1,report(%rip)
    mov (%rsp),%rax
    test %rax,%rax
    jz fail
    mov %rax,report+8(%rip)
    mov $1,%eax
    mov $1,%edi
    lea message(%rip),%rsi
    mov $message_end-message,%edx
    syscall
    cmp $message_end-message,%rax
    jne fail
    mov $60,%eax
    xor %edi,%edi
    syscall
fail:
    mov $60,%eax
    mov $92,%edi
    syscall
    ud2
message: .ascii "linux L4a main entered: hello via interpreter\n"
message_end:
.section .data
report: .quad 0,0
.section .note.GNU-stack,"",@progbits
