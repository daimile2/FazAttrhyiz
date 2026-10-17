/* Handwritten Linux x86-64 clone/futex probe. No libc/pthread dependency. */
.file "linux_threads.S"
.global _start
.section .text
_start:
    cmpq $2,(%rsp)
    jne basic
    mov 16(%rsp),%rax
    cmpb $108,(%rax)
    jne 1f
    movq $1,scenario(%rip)
1:  cmpb $103,(%rax)
    jne basic
    movq $2,scenario(%rip)
basic:
    movq $0x1234,-8(%rsp)
    mov %rsp,parent_sp(%rip)
    mov $39,%eax
    syscall
    mov %rax,parent_pid(%rip)
    mov $158,%eax
    mov $0x1002,%edi
    lea main_tls(%rip),%rsi
    syscall
    test %rax,%rax
    jne fail
    /* Explicit rejection/error checks, including two deliberate ENOSYS. */
    mov $56,%eax
    xor %edi,%edi
    syscall
    cmp $-38,%rax
    jne fail
    mov $202,%eax
    lea word(%rip),%rdi
    mov $2,%esi
    syscall
    cmp $-38,%rax
    jne fail
    mov $202,%eax
    mov $128,%esi
    mov $1,%edx
    xor %r10d,%r10d
    syscall
    cmp $-11,%rax
    jne fail
    mov $202,%eax
    lea word+1(%rip),%rdi
    syscall
    cmp $-22,%rax
    jne fail
    mov $202,%eax
    mov $-4,%rdi
    syscall
    cmp $-14,%rax
    jne fail
    mov $202,%eax
    lea word(%rip),%rdi
    xor %edx,%edx
    lea zero_time(%rip),%r10
    syscall
    cmp $-110,%rax
    jne fail
    mov $202,%eax
    lea short_time(%rip),%r10
    syscall
    cmp $-110,%rax
    jne fail
    cmpq $2,scenario(%rip)
    je capacity
    xor %r12d,%r12d
clone_retry:
    mov $56,%eax
    mov $0x1390f00,%edi
    lea stacks+8192(%rip),%rsi
    lea parent_tid(%rip),%rdx
    lea child_tid(%rip),%r10
    lea child_tls(%rip),%r8
    syscall
    cmp $-11,%rax
    jne cloned
    inc %r12d
    cmp $100000,%r12d
    jae fail
    mov $24,%eax
    syscall
    test %rax,%rax
    jne fail
    jmp clone_retry
cloned:
    test %rax,%rax
    jz child
    js fail
    cmp parent_tid(%rip),%eax
    jne fail
    cmpq $1,scenario(%rip)
    je leader_exit
    cmp child_tid(%rip),%eax
    jne fail
    mov $202,%eax
    lea word(%rip),%rdi
    mov $128,%esi
    xor %edx,%edx
    lea long_time(%rip),%r10
    syscall
    test %rax,%rax
    jne fail
join:
    mov child_tid(%rip),%edx
    test %edx,%edx
    jz joined
    mov $202,%eax
    lea child_tid(%rip),%rdi
    mov $128,%esi
    lea long_time(%rip),%r10
    syscall
    test %rax,%rax
    jz join
    cmp $-11,%rax
    je join
    jmp fail
joined:
    cmpq $1,result(%rip)
    jne fail
    cmpq $0x1122,%fs:0
    jne fail
    cmpq $0x1234,-8(%rsp)
    jne fail
    mov $1,%eax
    mov $1,%edi
    lea ok(%rip),%rsi
    mov $(ok_end-ok),%edx
    syscall
leader_exit:
    mov $60,%eax
    xor %edi,%edi
    syscall
    ud2
capacity:
    xor %r12d,%r12d
    lea stacks(%rip),%r13
more:
    add $8192,%r13
    mov $56,%eax
    mov $0x10f00,%edi
    mov %r13,%rsi
    syscall
    test %rax,%rax
    jz child
    js fail
    inc %r12d
    cmp $7,%r12d
    jb more
    mov $56,%eax
    mov $0x10f00,%edi
    syscall
    cmp $-11,%rax
    jne fail
    xor %r14d,%r14d
wait_parked:
    cmpq $7,parked_count(%rip)
    je terminate_group
    mov $24,%eax
    syscall
    test %rax,%rax
    jne fail
    inc %r14d
    cmp $100000,%r14d
    jb wait_parked
    jmp fail
terminate_group:
    mov $231,%eax
    mov $9,%edi
    syscall
    ud2
child:
    cmpq $2,scenario(%rip)
    jne normal_stack
    cmp %r13,%rsp
    jne fail
    mov %r12,-8(%rsp)
    jmp parked
normal_stack:
    lea stacks+8192(%rip),%rax
    cmp %rax,%rsp
    jne fail
    cmp parent_sp(%rip),%rsp
    je fail
    movq $0x5678,-8(%rsp)
    mov $39,%eax
    syscall
    cmp parent_pid(%rip),%rax
    jne fail
    mov $186,%eax
    syscall
    cmp child_tid(%rip),%eax
    jne fail
    cmpq $0x3344,%fs:0
    jne fail
    movq $1,result(%rip)
    cmpq $1,scenario(%rip)
    je orphan_writer
    xor %r12d,%r12d
wake:
    mov $202,%eax
    lea word(%rip),%rdi
    mov $129,%esi
    mov $1,%edx
    syscall
    cmp $1,%rax
    je child_exit
    test %rax,%rax
    jne fail
    inc %r12d
    cmp $1000000,%r12d
    jae fail
    mov $24,%eax
    syscall
    test %rax,%rax
    jne fail
    jmp wake
child_exit:
    mov $60,%eax
    xor %edi,%edi
    syscall
    ud2
orphan_writer:
    mov $24,%eax
    syscall
    test %rax,%rax
    jne fail
    mov $1,%eax
    mov $1,%edi
    lea leader_ok(%rip),%rsi
    mov $(leader_end-leader_ok),%edx
    syscall
    cmp $(leader_end-leader_ok),%rax
    jne fail
    jmp child_exit
parked:
    lock incq parked_count(%rip)
    mov $202,%eax
    lea word(%rip),%rdi
    mov $128,%esi
    xor %edx,%edx
    lea long_time(%rip),%r10
    syscall
    jmp child_exit
fail:
    mov $231,%eax
    mov $99,%edi
    syscall
    ud2
.section .rodata
ok: .ascii "linux threads: shared-mm TLS futex join ok\n"
ok_end:
leader_ok: .ascii "linux threads: leader-exit child-live ok\n"
leader_end:
.section .data
.balign 8
main_tls: .quad 0x1122
child_tls: .quad 0x3344
scenario: .quad 0
parent_pid: .quad 0
parent_sp: .quad 0
result: .quad 0
parked_count: .quad 0
word: .long 0
parent_tid: .long 0
child_tid: .long 0
.balign 8
zero_time: .quad 0,0
short_time: .quad 0,2000000
long_time: .quad 2,0
.section .bss
.balign 16
stacks: .skip 65536
.section .note.GNU-stack,"",@progbits
