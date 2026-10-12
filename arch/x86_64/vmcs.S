#ifndef CAPMIX_HOSTED
.section .text
/* VM exit returns to exactly the saved host stack. The guest only executes
 * VMCALL, but all SysV callee-saved registers are protected regardless. */
.globl cm_hw_vmlaunch_once
cm_hw_vmlaunch_once:
pushq %rbp
pushq %rbx
pushq %r12
pushq %r13
pushq %r14
pushq %r15
subq $8,%rsp
movq %rsp,%rsi
movq $0x6c14,%rdi
call cm_hw_vmwrite
testl %eax,%eax
jnz .Lrestore
leaq cm_vmexit_return(%rip),%rsi
movq $0x6c16,%rdi
call cm_hw_vmwrite
testl %eax,%eax
jnz .Lrestore
cm_launch_instruction:
vmlaunch
setc %al
setz %dl
movzbl %al,%eax
movzbl %dl,%edx
leal (%eax,%edx,2),%eax
jmp .Lrestore
cm_launch_fault:
movl $-1,%eax
jmp .Lrestore
.globl cm_vmexit_return
cm_vmexit_return:
/* This includes late VM-entry failures; C checks EXIT_REASON bit 31. */
movl $4,%eax
.Lrestore:
addq $8,%rsp
popq %r15
popq %r14
popq %r13
popq %r12
popq %rbx
popq %rbp
ret
.globl cm_guest_vmcall
cm_guest_vmcall:
vmcall
ud2
.section .rodata
.globl cm_vmcs_fixups_begin,cm_vmcs_fixups_end
cm_vmcs_fixups_begin:
.quad cm_launch_instruction,cm_launch_fault
cm_vmcs_fixups_end:
#endif
.section .note.GNU-stack,"",@progbits
