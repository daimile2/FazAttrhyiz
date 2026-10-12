/* Native CapMix syscall ABI, not Linux. Persisted marker is never truncated
 * before the previous-boot contents have been checked. */
.macro call nr
 movl $\nr,%eax
 syscall
.endm
.macro expect value
 cmpq $\value,%rax
 jne failed
.endm
.macro say label,length
 movl $1,%edi
 leaq \label(%rip),%rsi
 movl $\length,%edx
 call 2
.endm
.section .text
.global _start
_start:
 movw %cs,%ax
 andl $3,%eax
 cmp $3,%eax
 jne failed
 movabs $0xffffffff80000000,%rdi
 xorl %esi,%esi
 call 11
 expect -2
 movabs $0xffffffff80000000,%rdi
 call 14
 expect -2
 call 15
 expect -2
 leaq directory(%rip),%rdi
 call 14
 test %rax,%rax
 jz 1f
 expect -4
1:
 leaq path(%rip),%rdi
 xorl %esi,%esi
 call 11
 cmpq $-12,%rax
 je fresh
 test %rax,%rax
 js failed
 movq %rax,%r12
 movq %r12,%rdi
 leaq buffer(%rip),%rsi
 movl $marker_len,%edx
 call 12
 expect marker_len
 leaq marker(%rip),%rsi
 leaq buffer(%rip),%rdi
 movl $marker_len,%ecx
 repe cmpsb
 jne failed
 movq %r12,%rdi
 call 13
 expect 0
 say previous,previous_len
fresh:
 leaq path(%rip),%rdi
 movl $0x242,%esi
 call 11
 test %rax,%rax
 js failed
 movq %rax,%r12
 /* Invalid read destination (RX text), overflow, kernel pointer and >1MiB
  * must all fail before cursor changes or file writes. */
 movq %r12,%rdi
 leaq _start(%rip),%rsi
 movl $1,%edx
 call 12
 expect -2
 movq %r12,%rdi
 movq $-8,%rsi
 movl $16,%edx
 call 2
 expect -2
 movq %r12,%rdi
 leaq buffer(%rip),%rsi
 movl $1048577,%edx
 call 2
 expect -2
 movq %r12,%rdi
 movabs $0xffffffff80000000,%rsi
 movl $1,%edx
 call 12
 expect -2
 movq %r12,%rdi
 leaq marker(%rip),%rsi
 movl $marker_len,%edx
 call 2
 expect marker_len
 movq %r12,%rdi
 call 16
 expect 0
 movq %r12,%rdi
 call 13
 expect 0
 /* Cross-page write/read, then unlink temporary file. */
 leaq scratch(%rip),%rdi
 movl $0x242,%esi
 call 11
 test %rax,%rax
 js failed
 movq %rax,%r12
 leaq buffer(%rip),%rdi
 movl $4097,%ecx
 movb $0x5a,%al
 rep stosb
 movq %r12,%rdi
 leaq buffer(%rip),%rsi
 movl $4097,%edx
 call 2
 expect 4097
 movq %r12,%rdi
 call 16
 expect 0
 movq %r12,%rdi
 call 13
 expect 0
 leaq scratch(%rip),%rdi
 xorl %esi,%esi
 call 11
 test %rax,%rax
 js failed
 movq %rax,%r12
 /* FILE capability write denial on readonly handle. */
 movq %r12,%rdi
 leaq marker(%rip),%rsi
 movl $1,%edx
 call 2
 expect -3
 movq %r12,%rdi
 leaq readbuf(%rip),%rsi
 movl $4097,%edx
 call 12
 expect 4097
 leaq buffer(%rip),%rsi
 leaq readbuf(%rip),%rdi
 movl $4097,%ecx
 repe cmpsb
 jne failed
 movq %r12,%rdi
 call 13
 expect 0
 leaq scratch(%rip),%rdi
 call 15
 expect 0
 leaq scratch(%rip),%rdi
 xorl %esi,%esi
 call 11
 expect -12
 say success,success_len
 xorl %edi,%edi
 call 1
 ud2
failed:
 say failure,failure_len
 movl $1,%edi
 call 1
 ud2
.section .data
directory: .asciz "/p1a"
path: .asciz "/p1a/user.txt"
scratch: .asciz "/p1a/chunk.bin"
marker: .ascii "CapMix P1a ELF file persistence\n"
.set marker_len,.-marker
previous: .ascii "p1a previous boot payload verified\n"
.set previous_len,.-previous
success: .ascii "p1a file syscalls verified: copyin/out invalid-pointer rights chunk=4097 fsync unlink exit=0\n"
.set success_len,.-success
failure: .ascii "p1a file syscall FAIL\n"
.set failure_len,.-failure
.section .bss
.balign 4096
buffer: .skip 8192
readbuf: .skip 8192
.section .note.GNU-stack,"",@progbits
