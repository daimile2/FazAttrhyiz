.global _start
.section .text
_start:
    cmpq $2,(%rsp)
    jne fail
    mov 16(%rsp),%rax
    cmpb $'c',(%rax)
    jne fail
    mov $158,%eax
    mov $0x1003,%edi
    lea fs(%rip),%rsi
    syscall
    test %rax,%rax
    jne fail
    cmpq $0,fs(%rip)
    jne fail
    mov $5,%eax
    mov $3,%edi
    lea fs(%rip),%rsi
    syscall
    cmp $-9,%rax
    jne fail
    mov $1,%eax
    mov $1,%edi
    lea ok(%rip),%rsi
    mov $(ok_end-ok),%edx
    syscall
    mov $60,%eax
    mov $42,%edi
    syscall
fail:
    mov $60,%eax
    mov $98,%edi
    syscall
.section .rodata
ok: .ascii "linux L2 exec target argv FS CLOEXEC verified\n"
ok_end:
.section .data
fs: .zero 144
.section .note.GNU-stack,"",@progbits
