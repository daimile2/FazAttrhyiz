#include "entry_asm.h"
.section .entry.text,"ax",@progbits
    .global cm_irq_stub_kbd

/* Minimal ISR stub: save the volatile registers the C handler may clobber,
 * call the C top half, restore, iretq. Mirrors the calling convention used
 * by the existing timer stub. EOI is sent from cm_ioapic_kbd_isr(). */
cm_irq_stub_kbd:
    CM_ENTRY_SAVE
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    call  cm_ioapic_kbd_isr
    popq  %r11
    popq  %r10
    popq  %r9
    popq  %r8
    popq  %rdi
    popq  %rsi
    popq  %rdx
    popq  %rcx
    popq  %rax
    CM_ENTRY_RESTORE
    iretq
/* Dedicated NIC entry. Preserve incoming CR3 using the existing KPTI
 * trampoline convention; align the interrupted stack before calling C. */
.globl cm_irq_stub_e1000
.type cm_irq_stub_e1000,@function
cm_irq_stub_e1000:
    CM_ENTRY_SAVE
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    movq %rsp,%r12
    andq $-16,%rsp
    cld
    call cm_e1000_irq_isr
    movq %r12,%rsp
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rax
    CM_ENTRY_RESTORE
    iretq
.globl cm_irq_stub_vnet
.type cm_irq_stub_vnet,@function
cm_irq_stub_vnet:
    CM_ENTRY_SAVE
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    movq %rsp,%r12
    andq $-16,%rsp
    cld
    call cm_virtio_net_irq_isr
    movq %r12,%rsp
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rax
    CM_ENTRY_RESTORE
    iretq
/* Silences "missing .note.GNU-stack section implies executable stack" and
 * keeps this object from being marked as requiring an executable stack. */
.section .note.GNU-stack,"",@progbits
