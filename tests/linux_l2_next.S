.global _start
.section .text
_start:
    call compat_check
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

.section .text
compat_check:
    mov $14,%eax
    xor %edi,%edi
    xor %esi,%esi
    lea compat_old(%rip),%rdx
    mov $8,%r10d
    syscall
    test %rax,%rax
    jne fail
    cmpq $2,compat_old(%rip)
    jne fail
    mov $13,%eax
    mov $10,%edi
    syscall
    test %rax,%rax
    jne fail
    cmpq $1,compat_old(%rip)
    jne fail
    ret
.section .data
compat_mask: .quad 2
compat_action: .quad 1,0,0,0
compat_old: .zero 32
