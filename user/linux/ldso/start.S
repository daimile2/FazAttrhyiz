.global _start
.hidden rtld_main
.section .text
_start:
    mov %rsp,%r12
    mov %rsp,%rdi
    and $-16,%rsp
    call rtld_main
    mov %r12,%rsp
    xor %edx,%edx
    jmp *%rax
.section .note.GNU-stack,"",@progbits
/* Protocol marker, not a signature: kernel must not enter old rtld code which
 * predates the required RW+NX -> RX finalization contract. */
.section .note.capmix.wx,"a",@note
.balign 4
.long 9,4,1
.asciz "CapMixWX"
.balign 4
.long 1
