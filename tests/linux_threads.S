#include "../include/capmix/fd_limits.h"
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
1:  cmpb $114,(%rax)
    jne 2f
    movq $3,scenario(%rip)
2:  cmpb $103,(%rax)
    jne basic
    movq $2,scenario(%rip)
basic:
    movq $0x1234,-512(%rsp)
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
    cmpq $3,scenario(%rip)
    je reuse
    call prepare_files
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
    /* A pipe may block here; its writer must still enter the FILE gate. */
    mov $0,%eax
    mov pipefds(%rip),%edi
    lea pipe_byte(%rip),%rsi
    mov $1,%edx
    syscall
    cmp $1,%rax
    jne fail
    cmpb $112,pipe_byte(%rip)
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
    call parent_files
    cmpq $1,result(%rip)
    jne fail
    cmpq $0x1122,%fs:0
    jne fail
    cmpq $0x1234,-512(%rsp)
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
/* More than the lifetime cap: reuse the same stack/TLS/clear-tid address. */
reuse:
    xor %r13d,%r13d
    mov $1,%r14d
reuse_round:
    movq $0,result(%rip)
    xor %r12d,%r12d
reuse_retry:
    mov $56,%eax
    mov $0x1390f00,%edi
    lea stacks+8192(%rip),%rsi
    lea parent_tid(%rip),%rdx
    lea child_tid(%rip),%r10
    lea child_tls(%rip),%r8
    syscall
    test %rax,%rax
    jz reuse_child
    cmp $-11,%rax
    jne reuse_created
    inc %r12d
    cmp $100000,%r12d
    jae fail
    mov $24,%eax
    syscall
    jmp reuse_retry
reuse_created:
    test %rax,%rax
    js fail
    cmp %r13,%rax
    jbe fail
    mov %rax,%r13
    cmp parent_tid(%rip),%eax
    jne fail
reuse_join:
    mov child_tid(%rip),%edx
    test %edx,%edx
    jz reuse_joined
    mov $202,%eax
    lea child_tid(%rip),%rdi
    mov $128,%esi
    lea long_time(%rip),%r10
    syscall
    test %rax,%rax
    jz reuse_join
    cmp $-11,%rax
    je reuse_join
    jmp fail
reuse_joined:
    cmp %r14,result(%rip)
    jne fail
    cmpq $0x1122,%fs:0
    jne fail
    inc %r14
    cmp $33,%r14
    jb reuse_round
    /* Reuse never unfreezes the group's mutation/migration restrictions. */
    mov $57,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $59,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $9,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $203,%eax
    xor %edi,%edi
    mov $8,%esi
    lea reuse_mask(%rip),%rdx
    syscall
    cmp $-95,%rax
    jne fail
    mov $1,%eax
    mov $1,%edi
    lea reuse_ok(%rip),%rsi
    mov $(reuse_ok_end-reuse_ok),%edx
    syscall
    jmp leader_exit
reuse_child:
    lea stacks+8192(%rip),%rax
    cmp %rax,%rsp
    jne fail
    cmpq $0x3344,%fs:0
    jne fail
    mov $186,%eax
    syscall
    cmp child_tid(%rip),%eax
    jne fail
    mov $218,%eax
    lea child_tid(%rip),%rdi
    syscall
    cmp parent_tid(%rip),%eax
    jne fail
    mov %r14,result(%rip)
    jmp child_exit
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
    mov $901,%r15d
    cmpq $2,scenario(%rip)
    jne normal_stack
    cmp %r13,%rsp
    jne fail
    mov %r12,-8(%rsp)
    jmp parked
normal_stack:
    mov $902,%r15d
    lea stacks+8192(%rip),%rax
    cmp %rax,%rsp
    jne fail
    cmp parent_sp(%rip),%rsp
    je fail
    movq $0x5678,-512(%rsp)
    mov $903,%r15d
    mov $39,%eax
    syscall
    cmp parent_pid(%rip),%rax
    jne fail
    mov $904,%r15d
    mov $186,%eax
    syscall
    cmp child_tid(%rip),%eax
    jne fail
    mov $905,%r15d
    cmpq $0x3344,%fs:0
    jne fail
    call child_files
    mov $906,%r15d
    cmpq $0x5678,-512(%rsp)
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
/* Syscall tests deliberately retry EAGAIN: it is the bounded group gate's
 * observable contention policy, not a hidden busy spin in the kernel. */
fd_syscall:
    push %r12
    push %r13
    mov %rax,%r12
    xor %r13d,%r13d
1:  mov %r12,%rax
    syscall
    cmp $-11,%rax
    jne 2f
    inc %r13d
    cmp $100000,%r13d
    jae fail
    mov $24,%eax
    syscall
    jmp 1b
2:  pop %r13
    pop %r12
    ret
prepare_files:
    /* Unique pathname per TGID, including the two simultaneous AP groups. */
    lea file_path(%rip),%rdi
    lea hex_digits(%rip),%rsi
    mov parent_pid(%rip),%rax
    mov $8,%ecx
1:  mov %eax,%edx
    and $15,%edx
    mov (%rsi,%rdx),%dl
    mov %dl,10(%rdi,%rcx)
    shr $4,%eax
    loop 1b
    mov $2,%eax
    lea file_path(%rip),%rdi
    mov $578,%esi
    syscall
    cmp $3,%rax
    jl fail
    mov %rax,file_fd(%rip)
    mov %rax,%rdi
    mov $1,%eax
    lea file_data(%rip),%rsi
    mov $6,%edx
    syscall
    cmp $6,%rax
    jne fail
    mov $8,%eax
    xor %esi,%esi
    xor %edx,%edx
    syscall
    test %rax,%rax
    jne fail
    mov $32,%eax
    syscall
    cmp $3,%rax
    jl fail
    mov %rax,spare_fd(%rip)
    mov $22,%eax
    lea pipefds(%rip),%rdi
    syscall
    test %rax,%rax
    jne fail
    ret
child_files:
    mov $33,%eax
    mov file_fd(%rip),%rdi
    mov $10,%esi
    mov $101,%r15d
    call fd_syscall
    cmp $10,%rax
    jne fail
    mov $3,%eax
    mov spare_fd(%rip),%rdi
    mov $102,%r15d
    call fd_syscall
    test %rax,%rax
    jne fail
    mov $0,%eax
    mov $10,%edi
    lea file_bytes(%rip),%rsi
    mov $2,%edx
    mov $103,%r15d
    call fd_syscall
    cmp $2,%rax
    jne fail
    cmpw $0x6261,file_bytes(%rip)
    jne fail
    /* Reject stdio target/close, pipe source/target, and invalid fd. */
    mov $3,%eax
    mov $1,%edi
    mov $104,%r15d
    call fd_syscall
    cmp $-95,%rax
    jne fail
    mov $33,%eax
    mov file_fd(%rip),%rdi
    xor %esi,%esi
    mov $105,%r15d
    call fd_syscall
    cmp $-95,%rax
    jne fail
    mov $33,%eax
    mov pipefds(%rip),%edi
    mov $12,%esi
    mov $106,%r15d
    call fd_syscall
    cmp $-95,%rax
    jne fail
    mov $33,%eax
    mov file_fd(%rip),%rdi
    mov pipefds+4(%rip),%esi
    mov $107,%r15d
    call fd_syscall
    cmp $-95,%rax
    jne fail
    mov $3,%eax
    mov pipefds(%rip),%edi
    mov $108,%r15d
    call fd_syscall
    cmp $-95,%rax
    jne fail
    mov $3,%eax
    mov $31,%edi
    mov $109,%r15d
    call fd_syscall
    cmp $-9,%rax
    jne fail
    /* Existing policy remains frozen even after a thread has exited. */
    mov $110,%r15d
    mov $57,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $111,%r15d
    mov $59,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $112,%r15d
    mov $9,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $113,%r15d
    mov $12,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $114,%r15d
    mov $11,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $115,%r15d
    mov $10,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $116,%r15d
    mov $2,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $117,%r15d
    mov $72,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $118,%r15d
    mov $22,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $119,%r15d
    mov $41,%eax
    syscall
    cmp $-95,%rax
    jne fail
    /* Pipe I/O is still usable and must not hold the FILE mutation gate. */
    mov $1,%eax
    mov pipefds+4(%rip),%edi
    lea pipe_data(%rip),%rsi
    mov $1,%edx
    syscall
    cmp $1,%rax
    jne fail
    ret
parent_files:
    /* Group restrictions persist after the child exits. */
    mov $57,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $59,%eax
    syscall
    cmp $-95,%rax
    jne fail
    mov $9,%eax
    syscall
    cmp $-95,%rax
    jne fail
    /* The child closed the parent's spare alias; its own alias survived exit. */
    mov $0,%eax
    mov $10,%edi
    lea file_bytes(%rip),%rsi
    mov $2,%edx
    call fd_syscall
    cmp $2,%rax
    jne fail
    cmpw $0x6463,file_bytes(%rip)
    jne fail
    mov $8,%eax
    mov file_fd(%rip),%rdi
    xor %esi,%esi
    mov $1,%edx
    call fd_syscall
    cmp $4,%rax
    jne fail
    mov $33,%eax
    mov spare_fd(%rip),%rdi
    mov $12,%esi
    call fd_syscall
    cmp $-9,%rax
    jne fail
    /* Replace FILE alias with console, then FILE again. writev -> write
     * re-enters the gate for the same TID; this tests recursive ownership. */
    mov $33,%eax
    mov $1,%edi
    mov $10,%esi
    call fd_syscall
    cmp $10,%rax
    jne fail
    mov $20,%eax
    mov $10,%edi
    lea vector(%rip),%rsi
    mov $2,%edx
    call fd_syscall
    cmp $(fd_msg_end-fd_msg),%rax
    jne fail
    mov $33,%eax
    mov file_fd(%rip),%rdi
    mov $10,%esi
    call fd_syscall
    cmp $10,%rax
    jne fail
    mov $33,%eax
    mov $10,%edi
    mov $10,%esi
    call fd_syscall
    cmp $10,%rax
    jne fail
    /* Exhaust only the fd table with console aliases; never overwrite a slot. */
    push %r14
    push %r15
    xor %r14d,%r14d
    lea fill_fds(%rip),%r15
fill_more:
    mov $32,%eax
    mov $1,%edi
    call fd_syscall
    cmp $-24,%rax
    je fill_done
    cmp $3,%rax
    jl fail
    cmp $CM_FD_MAX,%r14
    jae fail
    mov %rax,(%r15,%r14,8)
    inc %r14
    jmp fill_more
fill_done:
    cmp $(CM_FD_MAX-12),%r14
    jb fail
empty_more:
    dec %r14
    mov (%r15,%r14,8),%rdi
    mov $3,%eax
    call fd_syscall
    test %rax,%rax
    jne fail
    test %r14,%r14
    jne empty_more
    pop %r15
    pop %r14
    mov $3,%eax
    mov $10,%edi
    call fd_syscall
    test %rax,%rax
    jne fail
    /* Last alias close must leave original open and at the shared offset. */
    mov $0,%eax
    mov file_fd(%rip),%rdi
    lea file_bytes(%rip),%rsi
    mov $2,%edx
    call fd_syscall
    cmp $2,%rax
    jne fail
    cmpw $0x6665,file_bytes(%rip)
    jne fail
    mov $3,%eax
    mov file_fd(%rip),%rdi
    call fd_syscall
    test %rax,%rax
    jne fail
    mov $1,%eax
    mov $1,%edi
    lea fd_ok(%rip),%rsi
    mov $(fd_ok_end-fd_ok),%edx
    call fd_syscall
    cmp $(fd_ok_end-fd_ok),%rax
    jne fail
    ret

fail:
    mov %rax,%r8
    lea hex_digits(%rip),%rsi
    lea diagnostic_stage(%rip),%rdi
    mov %r15,%rax
    mov $8,%ecx
1:  mov %eax,%edx
    and $15,%edx
    mov (%rsi,%rdx),%dl
    mov %dl,-1(%rdi,%rcx)
    shr $4,%rax
    loop 1b
    lea diagnostic_rax(%rip),%rdi
    mov %r8,%rax
    mov $16,%ecx
1:  mov %eax,%edx
    and $15,%edx
    mov (%rsi,%rdx),%dl
    mov %dl,-1(%rdi,%rcx)
    shr $4,%rax
    loop 1b
    mov $1,%eax
    mov $1,%edi
    lea diagnostic(%rip),%rsi
    mov $(diagnostic_end-diagnostic),%edx
    syscall
    mov $231,%eax
    mov $99,%edi
    syscall
    ud2
.section .rodata
ok: .ascii "linux threads: shared-mm TLS futex join ok\n"
ok_end:
leader_ok: .ascii "linux threads: leader-exit child-live ok\n"
leader_end:
hex_digits: .ascii "0123456789abcdef"
file_data: .ascii "abcdef"
pipe_data: .ascii "p"
fd_msg: .ascii "linux thread fd: "
fd_msg_mid: .ascii "recursive console writev ok\n"
fd_msg_end:
fd_ok: .ascii "linux thread fd: shared dup/close/offset/full/reject ok\n"
fd_ok_end:
.section .data
.balign 8
diagnostic: .ascii "linux threads failure stage="
diagnostic_stage: .ascii "00000000 rax="
diagnostic_rax: .ascii "0000000000000000\n"
diagnostic_end:
file_path: .asciz "/thread-fd-00000000.txt"
.balign 8
file_fd: .quad 0
spare_fd: .quad 0
pipefds: .long 0,0
file_bytes: .short 0
pipe_byte: .byte 0
.balign 8
vector: .quad fd_msg,fd_msg_mid-fd_msg,fd_msg_mid,fd_msg_end-fd_msg_mid
fill_fds: .skip CM_FD_MAX*8
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
reuse_mask: .quad 1
reuse_ok: .ascii "linux threads reuse: rounds=32 joined=32 TID-monotonic TLS/stack/clear-tid verified; mutation/migration denied\n"
reuse_ok_end:
.section .bss
.balign 16
stacks: .skip 65536
.section .note.GNU-stack,"",@progbits
