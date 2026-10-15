/* Static x86_64 CapMix crt: argc/argv/envp from initial stack, exit=1.
 * _start RSP is 16-aligned; CALL gives main the SysV alignment (RSP%16=8).
 * Registers other than RSP are not a process-entry argument ABI. */
.section .text
.global _start
.type _start,@function
_start:
    xorl %ebp,%ebp
    cld
    movq (%rsp),%rdi
    leaq 8(%rsp),%rsi
    leaq 16(%rsp,%rdi,8),%rdx
    andq $-16,%rsp
    call main
    movslq %eax,%rdi
    movl $1,%eax
    syscall
    ud2
.size _start,.-_start
.section .note.GNU-stack,"",@progbits
