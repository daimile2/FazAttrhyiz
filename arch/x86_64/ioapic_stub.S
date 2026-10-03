.text
    .global cm_irq_stub_kbd

/* Minimal ISR stub: save the volatile registers the C handler may clobber,
 * call the C top half, restore, iretq. Mirrors the calling convention used
 * by the existing timer stub. EOI is sent from cm_ioapic_kbd_isr(). */
cm_irq_stub_kbd:
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
    iretq
/* Silences "missing .note.GNU-stack section implies executable stack" and
 * keeps this object from being marked as requiring an executable stack. */
.section .note.GNU-stack,"",@progbits
