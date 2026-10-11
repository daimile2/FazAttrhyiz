#ifndef CAPMIX_SYSCALL_H
#define CAPMIX_SYSCALL_H

#include "types.h"

/* Phase 2 ABI.
 * Native surface is capability-shaped. POSIX numbers sit in a high band
 * and go through fdio (conflict #3 — never unify the two primitives).
 */

#define CM_SYS_NOP            0
#define CM_SYS_EXIT           1
#define CM_SYS_WRITE          2
#define CM_SYS_SPAWN          3
#define CM_SYS_HANDLE_CLOSE   4
#define CM_SYS_CHANNEL_CREATE 5
#define CM_SYS_IPC_SEND       6
#define CM_SYS_IPC_RECV       7
#define CM_SYS_CLOCK          8
#define CM_SYS_TRACE_NEW      9
#define CM_SYS_COMMIT_STAT   10

/* File I/O. Native shape is still capability: open() mints a CM_OBJ_FILE
 * and installs a handle whose rights follow the open mode; the fd is only
 * an fdio index onto that handle. read/write check the HANDLE's rights. */
#define CM_SYS_OPEN          11
#define CM_SYS_READ          12
#define CM_SYS_CLOSE         13
#define CM_SYS_MKDIR         14
#define CM_SYS_UNLINK        15
#define CM_SYS_FSYNC         16

/* Bounded lo UDP capabilities (native API, not BSD/POSIX socket numbers). */
#define CM_SYS_UDP_OPEN      17
#define CM_SYS_UDP_BIND      18
#define CM_SYS_UDP_SEND      19
#define CM_SYS_UDP_RECV      20
#define CM_SYS_UDP_CLOSE     21
#define CM_SYS_TRACE_READ    22
#define CM_SYS_TRACE_CONFIG  23

/* POSIX compat band — routed, not native. */
#define CM_SYS_FORK         128
#define CM_SYS_EXECVE       129

typedef int64_t (*cm_sysfn_t)(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

int64_t cm_syscall(uint32_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
void    cm_syscall_init(void);
const char *cm_sys_name(uint32_t nr);

#endif
