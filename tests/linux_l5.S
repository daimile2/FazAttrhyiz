.global _start
.section .text
_start:
    call file_probe
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
/* Read-only file snapshot via actual Linux syscalls, including fork inheritance. */
file_probe:
    /* Make the probe independently runnable after installation, too. */
    mov $2,%eax
    lea file_path(%rip),%rdi
    mov $0x241,%esi
    xor %edx,%edx
    syscall
    test %rax,%rax
    js fail
    mov %rax,%r15
    mov %rax,%rdi
    mov $1,%eax
    lea file_payload(%rip),%rsi
    mov $5000,%edx
    syscall
    cmp $5000,%rax
    jne fail
    mov $3,%eax
    mov %r15,%rdi
    syscall
    test %rax,%rax
    jne fail
    mov $2,%eax
    lea file_path(%rip),%rdi
    xor %esi,%esi
    xor %edx,%edx
    syscall
    test %rax,%rax
    js fail
    mov %rax,%r15
    mov $9,%eax
    xor %edi,%edi
    mov $8192,%esi
    mov $1,%edx
    mov $2,%r10d
    mov %r15,%r8
    xor %r9d,%r9d
    syscall
    test %rax,%rax
    js fail
    mov %rax,%r12
    cmpb $65,(%r12)
    jne fail
    cmpb $66,4096(%r12)
    jne fail
    cmpb $0,5000(%r12)
    jne fail
    cmpb $0,8191(%r12)
    jne fail
    /* Unsupported writable/private and shared are explicit EOPNOTSUPP. */
    mov $9,%eax
    xor %edi,%edi
    mov $4096,%esi
    mov $3,%edx
    syscall
    cmp $-95,%rax
    jne fail
    mov $9,%eax
    mov $1,%edx
    mov $1,%r10d
    syscall
    cmp $-95,%rax
    jne fail
    mov $9,%eax
    mov $2,%r10d
    mov $1,%r9d
    syscall
    cmp $-22,%rax
    jne fail
    mov $9,%eax
    xor %r9d,%r9d
    mov $-1,%r8
    syscall
    cmp $-9,%rax
    jne fail
    /* A nonzero page-aligned file offset and unchanged fd seek position. */
    mov $9,%eax
    mov %r15,%r8
    mov $4096,%r9d
    syscall
    test %rax,%rax
    js fail
    cmpb $66,(%rax)
    jne fail
    cmpb $0,904(%rax)
    jne fail
    mov %rax,%rdi
    mov $11,%eax
    mov $4096,%esi
    syscall
    test %rax,%rax
    jne fail
    mov $8,%eax
    mov %r15,%rdi
    xor %esi,%esi
    mov $1,%edx
    syscall
    test %rax,%rax
    jne fail
    mov $3,%eax
    mov %r15,%rdi
    syscall
    test %rax,%rax
    jne fail
    mov $57,%eax
    syscall
    test %rax,%rax
    js fail
    jz file_child
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
    cmpb $65,(%r12)
    jne fail
    mov $11,%eax
    mov %r12,%rdi
    mov $8192,%esi
    syscall
    test %rax,%rax
    jne fail
    mov $1,%eax
    mov $1,%edi
    lea file_message(%rip),%rsi
    mov $file_message_end-file_message,%edx
    syscall
    ret
file_child:
    cmpb $66,4096(%r12)
    jne fail
    cmpb $0,5000(%r12)
    jne fail
    xor %edi,%edi
    jmp finish
file_path: .asciz "/data/mmap-private.bin"
file_message: .ascii "linux file mmap user: open/map/read offset/zero-tail close/fork/unmap write/shared/badfd rejected ok\n"
file_message_end:
message: .ascii "linux L5 user mmap: 1MiB touch + COW fork + unmap ok\n"
message_end:
.section .data
status: .long 0
.section .note.GNU-stack,"",@progbits

.section .data
file_payload: .fill 4096,1,65
    .fill 904,1,66
