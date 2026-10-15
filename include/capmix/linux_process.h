#ifndef CAPMIX_LINUX_PROCESS_H
#define CAPMIX_LINUX_PROCESS_H
#include "types.h"
/* Exactly the syscall entry's stack order; RCX/R11 are SYSCALL-clobbered
 * return RIP/RFLAGS. rsp is appended by C, not part of the 15-word save. */
typedef struct {uint64_t rax,rdi,rsi,rdx,r10,r8,r9,rcx,r11,r15,r14,r13,r12,rbx,rbp,rsp;} cm_linux_regs_t;
_Static_assert(offsetof(cm_linux_regs_t,r15)==72 && offsetof(cm_linux_regs_t,rsp)==120,"entry register frame");
int64_t cm_syscall_frame(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,cm_linux_regs_t *);
int64_t cm_linux_fork(void);
int64_t cm_linux_wait4(int64_t,uint64_t,uint64_t,uint64_t);
int64_t cm_linux_execve(uint64_t,uint64_t,uint64_t);
int cm_linux_cow_fault(uint64_t,uint64_t,uint64_t);
int cm_linux_l2_selftest(void);
void cm_usermode_replace_image(void);
#endif
