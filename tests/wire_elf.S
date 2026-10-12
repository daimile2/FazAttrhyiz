/* Explicit destination syscall 25(hid, &request, 0, 0).
 * First NIC is interface 0 in product; debug patches ifid before launch. */
.macro call nr
 movl $\nr,%eax
 syscall
.endm
.section .text
.global _start
_start:
 movw %cs,%ax
 andl $3,%eax
 cmp $3,%eax
 jne failed
 call 17
 test %rax,%rax
 jle failed
 movq %rax,%r12
 movq %r12,%rdi
 movl ifid(%rip),%esi
 movl $9700,%edx
 call 18
 test %rax,%rax
 jne failed
 /* Reject inaccessible send request before touching hardware. */
 movq %r12,%rdi
 movabs $0xffffffff80000000,%rsi
 xorl %edx,%edx
 xorl %r10d,%r10d
 call 25
 cmpq $-2,%rax
 jne failed
 movl $1,%r14d
again_size:
 leaq request+8(%rip),%rdi
 movl %r14d,%ecx
 movb $0x6d,%al
 rep stosb
 movw %r14w,request+6(%rip)
 call 8
 movq %rax,%r13
retry_send:
 movq %r12,%rdi
 leaq request(%rip),%rsi
 xorl %edx,%edx
 xorl %r10d,%r10d
 call 25
 cmpq %r14,%rax
 je sent
 cmpq $-5,%rax
 jne failed
 call check_timeout
 jmp retry_send
sent:
 call 8
 movq %rax,%r13
retry_recv:
 movq %r12,%rdi
 leaq datagram(%rip),%rsi
 call 20
 cmpq %r14,%rax
 je received
 cmpq $-5,%rax
 jne failed
 call check_timeout
 jmp retry_recv
received:
 cmpl $0x0a000202,datagram(%rip)
 jne failed
 cmpw $9999,datagram+8(%rip)
 jne failed
 cmpw $9700,datagram+10(%rip)
 jne failed
 leaq request+8(%rip),%rsi
 leaq datagram+16(%rip),%rdi
 movl %r14d,%ecx
 repe cmpsb
 jne failed
 cmp $1,%r14d
 jne 1f
 movl $257,%r14d
 jmp again_size
1:
 cmp $257,%r14d
 jne complete
 movl $1472,%r14d
 jmp again_size
complete:
 movq %r12,%rdi
 call 21
 test %rax,%rax
 jne failed
 movl $1,%edi
 leaq hello(%rip),%rsi
 movl $hello_len,%edx
 call 2
 xorl %edi,%edi
 call 1
 ud2
check_timeout:
 call 8
 subq %r13,%rax
 cmpq timeout(%rip),%rax
 ja failed
 ret
failed:
 movl $1,%edi
 call 1
 ud2
.section .data
ifid: .long 0
.balign 8
timeout: .quad 15000000000
request: .long 0x0a000202
 .word 9999,1
 .fill 1472,1,0
hello: .ascii "p1b udp wire user verified: CPL=3 bytes=1/257/1472 bg RX exit=0\n"
.set hello_len,.-hello
.section .bss
.balign 16
datagram: .skip 1488
.section .note.GNU-stack,"",@progbits
