/* Native ABI 26=open,27=connect,28=send,29=recv,30=close. */
.macro call nr
 movl $\nr,%eax
 syscall
.endm
.macro expect value
 cmpq $\value,%rax
 jne failed
.endm
.macro say label,len
 movl $1,%edi
 leaq \label(%rip),%rsi
 movl $\len,%edx
 call 2
.endm
.section .text
.global _start
_start:
 movw %cs,%ax
 andl $3,%eax
 movl %eax,cpl(%rip)
 cmp $3,%eax
 jne failed
 xorl %edi,%edi
 xorl %esi,%esi
 xorl %edx,%edx
 xorl %r10d,%r10d
 call 26
 test %rax,%rax
 jle failed
 movq %rax,%r12
 movl readonly_handle(%rip),%edi
 test %edi,%edi
 jz 0f
 leaq sent(%rip),%rsi
 movl $1,%edx
 movl $100,%r10d
 call 28
 expect -3
 movl $1,rights_report(%rip)
0:
 xorl %edx,%edx
 xorl %r10d,%r10d
 /* Invalid request pointer does not initiate a connection. */
 movq %r12,%rdi
 movabs $0xffffffff80000000,%rsi
 call 27
 expect -2
 movq %r12,%rdi
 leaq request(%rip),%rsi
 call 27
 cmpq $-12,%rax
 je no_nic
 test %rax,%rax
 jne failed
 /* Both send and recv reject bad buffers before any stream side effects. */
 movq %r12,%rdi
 movabs $0xffffffff80000000,%rsi
 movl $1,%edx
 movl $3000,%r10d
 call 28
 expect -2
 movq %r12,%rdi
 leaq sent(%rip),%rsi
 movl $4097,%edx
 call 28
 expect -2
 movq %r12,%rdi
 leaq _start(%rip),%rsi
 movl $1,%edx
 xorl %r10d,%r10d
 call 29
 expect -2
 movq %r12,%rdi
 movq $-8,%rsi
 movl $16,%edx
 call 29
 expect -2
 /* A UDP endpoint cannot be used as a TCP handle. */
 xorl %edi,%edi
 xorl %esi,%esi
 xorl %edx,%edx
 xorl %r10d,%r10d
 call 17
 test %rax,%rax
 jle failed
 movq %rax,%rbx
 movq %rbx,%rdi
 leaq sent(%rip),%rsi
 movl $1,%edx
 movl $100,%r10d
 call 28
 expect -2
 movq %rbx,%rdi
 xorl %esi,%esi
 xorl %edx,%edx
 xorl %r10d,%r10d
 call 21
 expect 0
 /* Deterministic 2049-byte payload, repeated syscalls may return partial. */
 leaq sent(%rip),%rdi
 movl $2049,%ecx
 xorl %eax,%eax
1:
 movb %al,(%rdi)
 addb $19,%al
 incq %rdi
 loop 1b
 xorl %r14d,%r14d
send_more:
 movq %r12,%rdi
 leaq sent(%rip),%rsi
 addq %r14,%rsi
 movl $2049,%edx
 subq %r14,%rdx
 movl $3000,%r10d
 call 28
 test %rax,%rax
 jle failed
 addq %rax,%r14
 cmpq $2049,%r14
 jb send_more
 jne failed
 movl %r14d,sent_count(%rip)
 call 8
 movq %rax,%r13
 xorl %r15d,%r15d
recv_more:
 movq %r12,%rdi
 leaq received(%rip),%rsi
 addq %r15,%rsi
 movl $2049,%edx
 subq %r15,%rdx
 cmp $73,%edx
 jbe 2f
 movl $73,%edx
2:
 xorl %r10d,%r10d
 call 29
 cmpq $-5,%rax
 je wait_recv
 test %rax,%rax
 jle failed
 addq %rax,%r15
 cmpq $2049,%r15
 jb recv_more
 jne failed
 movl %r15d,received_count(%rip)
 leaq sent(%rip),%rsi
 leaq received(%rip),%rdi
 movl $2049,%ecx
 repe cmpsb
 jne failed
 movq %r12,%rdi
 movl $3000,%esi
 xorl %edx,%edx
 xorl %r10d,%r10d
 call 30
 expect 0
 /* Closed handle must be rejected, and no data consumed. */
 movq %r12,%rdi
 leaq received(%rip),%rsi
 movl $1,%edx
 call 29
 expect -2
 say verified,verified_len
 movl $1,verified_report(%rip)
 /* EXIT also reclaims a deliberately unclosed TCP object. */
 xorl %edi,%edi
 xorl %esi,%esi
 xorl %edx,%edx
 xorl %r10d,%r10d
 call 26
 test %rax,%rax
 jle failed
 xorl %edi,%edi
 call 1
 ud2
wait_recv:
 call 8
 subq %r13,%rax
 cmpq wait_ticks(%rip),%rax
 ja failed
 jmp recv_more
no_nic:
 movq %r12,%rdi
 movl $100,%esi
 xorl %edx,%edx
 xorl %r10d,%r10d
 call 30
 expect 0
 say skipped,skipped_len
 movl $2,verified_report(%rip)
 xorl %edi,%edi
 call 1
 ud2
failed:
 movq %rax,last_error(%rip)
 say failure,failure_len
 movl $1,%edi
 call 1
 ud2
.section .data
request: .long 0xffffffff,0x0a000202
 .word 9998,0
 .long 3000
wait_ticks: .quad 15000000000
cpl: .long 0
verified_report: .long 0
sent_count: .long 0
received_count: .long 0
last_error: .quad 0
readonly_handle: .long 0
rights_report: .long 0
verified: .ascii "tcp user ELF verified: CPL=3 bytes=2049 copyin/out typed-cap recv-partial FIN exit=0\n"
.set verified_len,.-verified
skipped: .ascii "tcp user ELF skipped: no NIC connect=NOENT close/reclaim ok exit=0\n"
.set skipped_len,.-skipped
failure: .ascii "tcp user ELF failed: see syscall/native tcp error; exit=1\n"
.set failure_len,.-failure
.section .bss
.balign 4096
sent: .skip 4096
received: .skip 4096
.section .note.GNU-stack,"",@progbits
