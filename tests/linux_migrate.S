/* Controlled migration probe. Data ABI consumed by boot/linux_migrate_test.c. */
.file "linux_migrate.S"
.section .text
.global _start
_start:
    cmpq $1,(%rsp)
    jbe 1f
    movq $1,wire(%rip)
1:
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
    call endpoints_open
    xor %r12d,%r12d
again:
    call endpoints_write
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
    call endpoints_read
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
    call endpoints_write
    movq $1,mask(%rip)
    call move
    test %rax,%rax
    jne fail
    call cpu
    cmpl $0,where(%rip)
    jne fail
    call endpoints_read
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
    mov $1,%eax
    mov $1,%edi
    lea endpoint_message(%rip),%rsi
    mov $endpoint_message_end-endpoint_message,%edx
    syscall
    cmpq $0,wire(%rip)
    je 2f
    mov $1,%eax
    mov $1,%edi
    lea tcp_message(%rip),%rsi
    mov $tcp_message_end-tcp_message,%edx
    syscall
2:
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
endpoints_open:
    movq $10,report(%rip)
    mov $22,%eax
    lea pipefds(%rip),%rdi
    syscall
    test %rax,%rax
    jne fail
    movq $11,report(%rip)
    mov $41,%eax
    mov $2,%edi
    mov $2,%esi
    xor %edx,%edx
    syscall
    test %rax,%rax
    js fail
    movq $12,report(%rip)
    mov %rax,udpfd(%rip)
    mov %rax,%rdi
    mov $49,%eax
    lea udp_address(%rip),%rsi
    mov $16,%edx
    syscall
    test %rax,%rax
    jne fail
    movq $13,report(%rip)
    mov $42,%eax
    mov udpfd(%rip),%rdi
    syscall
    test %rax,%rax
    jne fail
    movq $14,report(%rip)
    mov $33,%eax
    mov udpfd(%rip),%rdi
    mov $20,%esi
    syscall
    cmp $20,%rax
    jne fail
    movq $15,report(%rip)
    mov $41,%eax
    mov $2,%edi
    mov $1,%esi
    xor %edx,%edx
    syscall
    test %rax,%rax
    js fail
    mov %rax,tcpfd(%rip)
    cmpq $0,wire(%rip)
    je 1f
    mov $42,%eax
    mov tcpfd(%rip),%rdi
    lea tcp_address(%rip),%rsi
    mov $16,%edx
    syscall
    test %rax,%rax
    jne fail
1:  ret
endpoints_write:
    movq $20,report(%rip)
    mov $1,%eax
    mov pipefds+4(%rip),%edi
    lea payload(%rip),%rsi
    mov $8,%edx
    syscall
    cmp $8,%rax
    jne fail
    movq $21,report(%rip)
    mov $1,%eax
    mov $20,%edi
    syscall
    cmp $8,%rax
    jne fail
    cmpq $0,wire(%rip)
    je 1f
    mov $1,%eax
    mov tcpfd(%rip),%rdi
    syscall
    cmp $8,%rax
    jne fail
1:  ret
endpoints_read:
    movq $30,report(%rip)
    xor %eax,%eax
    mov pipefds(%rip),%edi
    lea received(%rip),%rsi
    mov $8,%edx
    syscall
    cmp $8,%rax
    jne fail
    mov received(%rip),%rax
    cmp payload(%rip),%rax
    jne fail
    xor %eax,%eax
    mov udpfd(%rip),%rdi
    syscall
    cmp $8,%rax
    jne fail
    mov received(%rip),%rax
    cmp payload(%rip),%rax
    jne fail
    cmpq $0,wire(%rip)
    je 2f
    xor %r14d,%r14d
1:  xor %eax,%eax
    mov tcpfd(%rip),%rdi
    lea received(%rip),%rsi
    add %r14,%rsi
    mov $8,%edx
    sub %r14,%rdx
    syscall
    test %rax,%rax
    jle fail
    add %rax,%r14
    cmp $8,%r14
    jb 1b
    mov received(%rip),%rax
    cmp payload(%rip),%rax
    jne fail
2:  ret
move:
    movq $40,report(%rip)
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
endpoint_message: .ascii "linux migration endpoints: pipe+UDP queued/dup cross-CPU read/write verified\n"
endpoint_message_end:
tcp_message: .ascii "linux migration TCP: connected endpoint cross-CPU payload verified\n"
tcp_message_end:
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
wire: .quad 0
pipefds: .long -1,-1
udpfd: .quad -1
tcpfd: .quad -1
payload: .quad 0x123456789abcdef
received: .quad 0
udp_address: .byte 2,0,0x4a,0x98,127,0,0,1,0,0,0,0,0,0,0,0
tcp_address: .byte 2,0,0x27,0x0e,10,0,2,2,0,0,0,0,0,0,0,0
/* Keep independent COW page: extra PT_LOAD data page within same segment. */
.org 0x1000
cow_word: .quad 0
.section .note.GNU-stack,"",@progbits
