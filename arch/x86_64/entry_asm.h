#include "capmix/config.h"
/* Preserve incoming CR3 on the entry stack, independently of GS state.
 * This also covers interrupts in the SWAPGS/CR3 transition windows. */
.macro CM_ENTRY_SAVE
#if CM_KPTI && !defined(CAPMIX_HOSTED)
    pushq %rax
    movq %cr3, %rax
    pushq %rax
    movq cm_entry_kernel_cr3(%rip), %rax
    testq %rax, %rax
    jz 991f
    movq %rax, %cr3
991:
    movq 8(%rsp), %rax
#endif
.endm
.macro CM_ENTRY_RESTORE
#if CM_KPTI && !defined(CAPMIX_HOSTED)
    popq %rax
    movq %rax, %cr3
    popq %rax
#endif
.endm
.macro CM_ENTRY_FAULT
#if CM_KPTI && !defined(CAPMIX_HOSTED)
    movq cm_entry_kernel_cr3(%rip), %rax
    testq %rax, %rax
    jz 992f
    movq %rax, %cr3
992:
#endif
.endm
