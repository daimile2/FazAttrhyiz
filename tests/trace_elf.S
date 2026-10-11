.section .text
.global _start
_start:
    movw %cs,%ax
    andl $3,%eax
    movq %rax,reported_cpl(%rip)
    movl $22,%eax
    movq denied(%rip),%rdi
    xorl %esi,%esi
    movabs $0xffffffff80000000,%rdx
    movq $1,stage(%rip)
    syscall
    movq %rax,result(%rip)
    cmpq $-3,%rax
    jne failed
    movl $23,%eax
    movq granted(%rip),%rdi
    xorl %esi,%esi
    movq $2,stage(%rip)
    syscall
    movq %rax,result(%rip)
    cmpq $-3,%rax
    jne failed
    movl $22,%eax
    movq granted(%rip),%rdi
    xorl %esi,%esi
    leaq _start(%rip),%rdx
    movq $3,stage(%rip)
    syscall
    movq %rax,result(%rip)
    cmpq $-2,%rax
    jne failed
    movl $22,%eax
    movq granted(%rip),%rdi
    xorl %esi,%esi
    leaq request(%rip),%rdx
    movq $4,stage(%rip)
    syscall
    movq %rax,result(%rip)
    testq %rax,%rax
    jne failed
    cmpq $0x88,request+80(%rip)
    jne failed
    cmpq $5,request+72(%rip)
    jne failed
    movl $2,%eax
    movl $1,%edi
    leaq message(%rip),%rsi
    movl $17,%edx
    movq $5,stage(%rip)
    syscall
    movq %rax,result(%rip)
    movl $1,%eax
    xorl %edi,%edi
    movq $6,stage(%rip)
    syscall
    movq %rax,result(%rip)
1:  jmp 1b
failed:
    movl $1,%eax
    movl $1,%edi
    syscall
    movq %rax,result(%rip)
    jmp 1b
message: .ascii "trace user rx ok\n\0"
/* WRITE count excludes the NUL: 17 bytes. */
.section .data
.align 8
granted: .quad 0
denied: .quad 0
request: .zero 104
stage: .quad 0
result: .quad 0
reported_cpl: .quad 0
.section .note.GNU-stack,"",@progbits
