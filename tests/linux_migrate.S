/* Controlled migration probe. Data ABI consumed by boot/linux_migrate_test.c. */
.file "linux_migrate.S"
.section .text
.global _start
_start:
    mov $158,%eax
    mov $0x1002,%edi
    lea tls(%rip),%rsi
    syscall
    test %rax,%rax
    jne fail
    /* Invalid mask and pointer must leave placement untouched. */
    movq $3,mask(%rip)
    call move
    cmp $-22,%rax
    jne fail
    mov $203,%eax
    xor %edi,%edi
    mov $8,%esi
    mov $-1,%rdx
    syscall
    cmp $-14,%rax
    jne fail
    call cpu
    cmpl $0,where(%rip)
    jne fail
    xor %r12d,%r12d
again:
    movq $2,mask(%rip)
    call move
    cmp $-11,%rax
    jne moved
    incq rollbacks(%rip)
    call cpu
    cmpl $0,where(%rip)
    jne fail
    call move
moved:
    test %rax,%rax
    jne fail
    call cpu
    cmpl $1,where(%rip)
    jne fail
    mov %fs:0,%rax
    cmp tls(%rip),%rax
    jne fail
    incq %fs:8
    movq %fs:8,%rax
    lea 1(%r12),%rdx
    cmp %rdx,%rax
    jne fail
    movq $0x5566,cow_word(%rip) /* first write to this backing after CPU change */
    mov $158,%eax
    mov $0x1003,%edi
    lea fs_report(%rip),%rsi
    syscall
    lea tls(%rip),%rax
    cmp fs_report(%rip),%rax
    jne fail
    test %r12,%r12
    jnz after_fork
    mov $57,%eax /* actual fork/COW on destination before returning to CPU0 */
    syscall
    test %rax,%rax
    js fail
    jz child
    mov %rax,%r13
    movq $1,mask(%rip)
    call move /* unreaped child forbids migration, even if already exited */
    cmp $-95,%rax
    jne fail
    mov $61,%eax
    mov %r13,%rdi
    lea status(%rip),%rsi
    xor %edx,%edx
    xor %r10d,%r10d
    syscall
    cmp %r13,%rax
    jne fail
    cmpl $0x700,status(%rip)
    jne fail
    cmpq $0x5566,cow_word(%rip)
    jne fail
    cmpq $1,%fs:8
    jne fail
after_fork:
    mov $24,%eax /* schedule/preempt on destination, then another syscall */
    syscall
    movq $1,mask(%rip)
    call move
    test %rax,%rax
    jne fail
    call cpu
    cmpl $0,where(%rip)
    jne fail
    movq %fs:8,%rax
    lea 1(%r12),%rdx
    cmp %rdx,%rax
    jne fail
    cmpq $0x5566,cow_word(%rip)
    jne fail
    mov %fs:0,%rax
    cmp tls(%rip),%rax
    jne fail
    inc %r12
    cmp $4,%r12
    jb again
    movq $1,report(%rip)
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
    ud2
move:
    mov $203,%eax
    xor %edi,%edi
    mov $8,%esi
    lea mask(%rip),%rdx
    syscall
    ret
cpu:
    mov $309,%eax
    lea where(%rip),%rdi
    xor %esi,%esi
    xor %edx,%edx
    syscall
    test %rax,%rax
    jne fail
    ret
child:
    movq $1,mask(%rip)
    call move /* fork child itself also remains pinned */
    cmp $-95,%rax
    jne fail
    movq $0x7788,cow_word(%rip)
    movq $99,%fs:8
    mov $60,%eax
    mov $7,%edi
    syscall
    ud2
fail:
    mov $60,%eax
    mov $42,%edi
    syscall
    ud2
.section .rodata
message: .ascii "linux migration user: CPU0/1 roundtrip FS/TLS syscall COW ok\n"
message_end:
.section .data
report: .quad 0
rollbacks: .quad 0
mask: .quad 0
where: .quad 0
fs_report: .quad 0
status: .quad 0
.org 0x80
tls: .quad 0x1122334455667788,0
/* Keep independent COW page: extra PT_LOAD data page within same segment. */
.org 0x1000
cow_word: .quad 0
.section .note.GNU-stack,"",@progbits
