#include "entry_asm.h"
#ifndef CAPMIX_HOSTED
/* Each potentially faulting instruction has an exact RIP/recovery pair.
 * Recovery returns -1. CF/ZF on VMX instructions are returned separately. */
.macro PAIR start, recover
.pushsection .rodata
.quad \start, \recover
.popsection
.endm
.section .text
.globl cm_hw_rdmsr
cm_hw_rdmsr:
movl %edi,%ecx
1: rdmsr
shlq $32,%rdx
orq %rdx,%rax
movq %rax,(%rsi)
xorl %eax,%eax
ret
2: movl $-1,%eax
ret
.globl cm_hw_wrmsr
cm_hw_wrmsr:
movl %edi,%ecx
movq %rsi,%rax
movq %rsi,%rdx
shrq $32,%rdx
3: wrmsr
xorl %eax,%eax
ret
4: movl $-1,%eax
ret
.globl cm_hw_cr4
cm_hw_cr4:
5: movq %rdi,%cr4
xorl %eax,%eax
ret
6: movl $-1,%eax
ret
.globl cm_hw_vmxon
cm_hw_vmxon:
7: vmxon (%rdi)
setc %al
setz %dl
movzbl %al,%eax
movzbl %dl,%edx
leal (%eax,%edx,2),%eax
ret
8: movl $-1,%eax
ret
.globl cm_hw_vmxoff
cm_hw_vmxoff:
9: vmxoff
setc %al
setz %dl
movzbl %al,%eax
movzbl %dl,%edx
leal (%eax,%edx,2),%eax
ret
10: movl $-1,%eax
ret
.globl cm_hw_cr0
cm_hw_cr0:
13: movq %rdi,%cr0
xorl %eax,%eax
ret
14: movl $-1,%eax
ret
.globl cm_hw_test_ud
cm_hw_test_ud:
11: ud2
xorl %eax,%eax
ret
12: movl $-1,%eax
ret

/* Incremental VMCS instruction wrappers; return CF|ZF<<1 or -1 on fault. */
.macro STATUS
setc %al
setz %dl
movzbl %al,%eax
movzbl %dl,%edx
leal (%eax,%edx,2),%eax
.endm
.globl cm_hw_vmclear
cm_hw_vmclear:
15: vmclear (%rdi)
STATUS
ret
16: movl $-1,%eax
ret
.globl cm_hw_vmptrld
cm_hw_vmptrld:
17: vmptrld (%rdi)
STATUS
ret
18: movl $-1,%eax
ret
.globl cm_hw_vmwrite
cm_hw_vmwrite:
19: vmwrite %rsi,%rdi
STATUS
ret
20: movl $-1,%eax
ret
.globl cm_hw_vmread
cm_hw_vmread:
21: vmread %rdi,%rcx
STATUS
testl %eax,%eax
jnz 22f
movq %rcx,(%rsi)
22: ret
23: movl $-1,%eax
ret

.globl cm_hw_invept
cm_hw_invept:
24: invept (%rsi),%rdi
STATUS
ret
25: movl $-1,%eax
ret
.section .rodata
.globl cm_hw_fixups_begin,cm_hw_fixups_end
cm_hw_fixups_begin:
PAIR 1b,2b
PAIR 3b,4b
PAIR 5b,6b
PAIR 7b,8b
PAIR 9b,10b
PAIR 11b,12b
PAIR 13b,14b
PAIR 15b,16b
PAIR 17b,18b
PAIR 19b,20b
PAIR 21b,23b
PAIR 24b,25b
cm_hw_fixups_end:
.section .entry.text,"ax",@progbits
.macro PROBE_FAULT vec, noerr
.globl cm_hw_fault_\vec
cm_hw_fault_\vec:
.if \noerr
pushq $0
.endif
CM_ENTRY_SAVE
pushq %rax
pushq %rcx
pushq %rdx
pushq %rbx
pushq %rbp
pushq %rsi
pushq %rdi
pushq %r8
pushq %r9
pushq %r10
pushq %r11
pushq %r12
pushq %r13
pushq %r14
pushq %r15
movq %rsp,%rbx
#if CM_KPTI
leaq 136(%rbx),%rsi
#else
leaq 120(%rbx),%rsi
#endif
movl $\vec,%edi
andq $-16,%rsp
call cm_hw_fault_recover
movq %rbx,%rsp
popq %r15
popq %r14
popq %r13
popq %r12
popq %r11
popq %r10
popq %r9
popq %r8
popq %rdi
popq %rsi
popq %rbp
popq %rbx
popq %rdx
popq %rcx
popq %rax
CM_ENTRY_RESTORE
addq $8,%rsp
iretq
.endm
PROBE_FAULT 6,1
PROBE_FAULT 13,0
#endif
.section .note.GNU-stack,"",@progbits
