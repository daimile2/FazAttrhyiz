.global _start
.section .text
_start:
    mov $39,%eax
    syscall
    mov %rax,%r14
    xor %edi,%edi
    mov $12,%eax
    syscall
    mov %rax,%r12
    lea 8192(%rax),%rdi
    mov $12,%eax
    syscall
    cmp %rdi,%rax
    jne fail
    movq $0x1122,(%r12)
    movq $0,8(%r12)
    movq $0x3344,4096(%r12)
    mov $158,%eax
    mov $0x1002,%edi
    lea 4096(%r12),%rsi
    syscall
    test %rax,%rax
    jne fail
    mov $2,%eax
    lea file(%rip),%rdi
    mov $0x80000,%esi
    syscall
    cmp $3,%rax
    jne fail
    mov $0x12345678,%ebx
    mov $0x76543210,%r13d
    mov $57,%eax
    syscall
    cmp $0x12345678,%rbx
    jne fail
    cmp $0x76543210,%r13
    jne fail
    test %rax,%rax
    js fail
    jz child
    mov %rax,%rbp
    movq $0xaa,(%r12) /* hardware #PF: parent becomes private */
    movq $0xaa,word(%rip)
    mov $61,%eax
    mov %rbp,%rdi
    lea status(%rip),%rsi
    xor %edx,%edx
    xor %r10d,%r10d
    syscall
    cmp %rbp,%rax
    jne fail
    cmpl $0x2a00,status(%rip)
    jne fail
    cmpq $0xaa,(%r12)
    jne fail
    cmpq $0,8(%r12) /* child's kernel copyout did not alter parent */
    jne fail
    cmpq $0xaa,word(%rip)
    jne fail
    cmpq $0x3344,%fs:0
    jne fail
    mov $0,%eax /* child's CLOEXEC close did not close parent's fd */
    mov $3,%edi
    lea 16(%r12),%rsi
    mov $5,%edx
    syscall
    cmp $5,%rax
    jne fail
    mov $3,%eax
    mov $3,%edi
    syscall
    mov $61,%eax
    mov %rbp,%rdi
    xor %esi,%esi
    xor %edx,%edx
    xor %r10d,%r10d
    syscall
    cmp $-10,%rax
    jne fail
    /* Shrink then regrow: returned pages must be zeroed. */
    mov $12,%eax
    mov %r12,%rdi
    syscall
    cmp %r12,%rax
    jne fail
    mov $12,%eax
    lea 8192(%r12),%rdi
    syscall
    cmp %rdi,%rax
    jne fail
    cmpq $0,(%r12)
    jne fail
    mov $1,%eax
    mov $1,%edi
    lea ok(%rip),%rsi
    mov $(ok_end-ok),%edx
    syscall
    mov $60,%eax
    xor %edi,%edi
    syscall
child:
    cmpq $0x1122,(%r12)
    jne fail
    cmpq $0x55,word(%rip)
    jne fail
    mov $158,%eax /* kernel copyout must also COW the heap */
    mov $0x1003,%edi
    lea 8(%r12),%rsi
    syscall
    test %rax,%rax
    jne fail
    lea 4096(%r12),%rax
    cmp %rax,8(%r12)
    jne fail
    movq $0xbb,(%r12)
    movq $0xbb,word(%rip)
    cmpq $0x3344,%fs:0
    jne fail
    mov $59,%eax
    lea absent(%rip),%rdi
    xor %esi,%esi
    xor %edx,%edx
    syscall
    cmp $-2,%rax
    jne fail
    cmpq $0xbb,(%r12)
    jne fail
    mov $59,%eax
    lea native(%rip),%rdi
    xor %esi,%esi
    xor %edx,%edx
    syscall
    cmp $-8,%rax
    jne fail
    mov $39,%eax
    syscall
    cmp %r14,%rax
    je fail
    mov $59,%eax
    lea target(%rip),%rdi
    lea args(%rip),%rsi
    xor %edx,%edx
    syscall
    jmp fail /* success cannot return to this image */
fail:
    mov $60,%eax
    mov $99,%edi
    syscall
.section .rodata
file: .asciz "/l2-file.txt"
absent: .asciz "/linux-l2-no-such-image"
native: .asciz "/bin/hello.elf"
target: .asciz "/bin/linux-l2-next.elf"
arg: .asciz "cowexec"
ok: .ascii "linux L2 fork COW brk exec wait verified\n"
ok_end:
.section .data
word: .quad 0x55
status: .long 0
.align 8
args: .quad target,arg,0
.section .note.GNU-stack,"",@progbits
