/* PIC interpreter handoff probe, no libc, relocations, DT_NEEDED or TLS. */
.global _start
.section .text
_start:
    mov %rsp,%r12
    mov %cs,%ax
    and $3,%eax
    cmp $3,%eax
    jne fail
    mov (%rsp),%rax
    test %rax,%rax
    jz fail
    cmp $32,%rax
    ja fail
    lea 16(%rsp,%rax,8),%rsi
    mov $33,%ecx
1:  lodsq
    test %rax,%rax
    jz 2f
    loop 1b
    jmp fail
2:  xor %r13d,%r13d
    xor %r14d,%r14d
    xor %r8d,%r8d
    mov $32,%ebx
3:  lodsq
    mov (%rsi),%rdx
    add $8,%rsi
    test %rax,%rax
    jz done
    cmp $9,%rax
    jne 4f
    mov %rdx,%r13
    or $1,%r8d
4:  cmp $7,%rax
    jne 5f
    mov %rdx,%r14
    or $2,%r8d
5:  cmp $6,%rax
    jne 6f
    cmp $4096,%rdx
    jne fail
    or $4,%r8d
6:  cmp $3,%rax
    jne 7f
    cmp $0x400040,%rdx
    jne fail
    or $8,%r8d
7:  cmp $4,%rax
    jne 8f
    cmp $56,%rdx
    jne fail
    or $16,%r8d
8:  cmp $5,%rax
    jne 9f
    cmp $3,%rdx
    jne fail
    or $32,%r8d
9:  cmp $25,%rax
    jne 10f
    cmp $0x7fc000,%rdx
    jb fail
    cmp $0x7ffff0,%rdx
    ja fail
    mov (%rdx),%rax
    mov 8(%rdx),%rax
    or $64,%r8d
10: dec %ebx
    jnz 3b
    jmp fail
done:
    cmp $127,%r8d
    jne fail
    cmp $0x600000,%r14
    jne fail
    cmp $0x400000,%r13
    jb fail
    cmp $0x600000,%r13
    jae fail
    lea _start(%rip),%rax
    sub %r14,%rax
    cmp $0x100000,%rax
    jae fail
    mov $1,%eax
    mov $1,%edi
    lea message(%rip),%rsi
    mov $message_end-message,%edx
    syscall
    cmp $message_end-message,%rax
    jne fail
    mov %r12,%rsp
    xor %ebp,%ebp
    xor %edx,%edx /* no rtld finalizer */
    jmp *%r13
fail:
    mov $60,%eax
    mov $91,%edi
    syscall
    ud2
message: .ascii "linux L4a interp entered: CPL3 auxv verified\n"
message_end:
.section .note.GNU-stack,"",@progbits
