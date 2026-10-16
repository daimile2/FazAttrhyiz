.global _start
.section .text
_start:
    mov $9,%eax
    xor %edi,%edi
    mov $1048576,%esi
    mov $3,%edx
    mov $0x22,%r10d
    mov $-1,%r8
    xor %r9d,%r9d
    syscall
    test %rax,%rax
    js fail
    mov %rax,%r12
    xor %ecx,%ecx
1:  cmpq $0,(%r12,%rcx)
    jne fail
    movq %rcx,(%r12,%rcx)
    add $4096,%rcx
    cmp $1048576,%rcx
    jb 1b
    mov $57,%eax
    syscall
    test %rax,%rax
    js fail
    jz child
    mov %rax,%rdi
    lea status(%rip),%rsi
    xor %edx,%edx
    xor %r10d,%r10d
    mov $61,%eax
    syscall
    test %rax,%rax
    js fail
    cmpl $0,status(%rip)
    jne fail
    cmpq $0,(%r12)
    jne fail
    mov $11,%eax
    mov %r12,%rdi
    mov $1048576,%esi
    syscall
    test %rax,%rax
    jne fail
    mov $1,%eax
    mov $1,%edi
    lea message(%rip),%rsi
    mov $message_end-message,%edx
    syscall
    xor %edi,%edi
    jmp finish
child:
    movq $42,(%r12)
    cmpq $4096,4096(%r12)
    jne fail
    xor %edi,%edi
    jmp finish
fail:
    mov $73,%edi
finish:
    mov $60,%eax
    syscall
    ud2
message: .ascii "linux L5 user mmap: 1MiB touch + COW fork + unmap ok\n"
message_end:
.section .data
status: .long 0
.section .note.GNU-stack,"",@progbits
