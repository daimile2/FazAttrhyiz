#ifndef CAPMIX_CONTROL_H
#define CAPMIX_CONTROL_H
#include "types.h"
#define CM_SYS_CONSOLE_READ 31
#define CM_SYS_CONSOLE_WRITE 32
#define CM_SYS_CHILD_WAIT 33
#define CM_SYS_YIELD 34
#define CM_SYS_READDIR 35
#define CM_SYS_BOOT_CONTROL 36
#define CM_SYS_POWEROFF 37
#define CM_SYS_CHILD_KILL 38
#define CM_SYS_TSC_HZ 39
#define CM_POWEROFF_FORCE 2u
#define CM_SPAWN_SHELL 1u
#define CM_SPAWN_CONSOLE 2u /* explicit foreground console delegation; child handle 1 */
#define CM_WAIT_NONBLOCK 1u
/* ELF syscall 3: (controller_handle, request*, result*, 0). */
typedef struct {uint64_t path,argv,envp;uint32_t flags;int32_t cpu;} cm_user_spawn_t;
typedef struct {uint64_t pid;int32_t handle,cpu;} cm_user_child_t;
typedef struct {uint64_t pid;int64_t exit_code;uint64_t preempts;int32_t cpu,reserved;} cm_user_wait_t;
typedef struct {uint32_t disk,cpus;char backend[24],nic[16];uint32_t nic_up,version;} cm_user_boot_t;
struct cm_proc;
int cm_control_seed(struct cm_proc *p);
int64_t cm_control_syscall(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t);
void cm_control_process_exit(struct cm_proc *p);
int cm_control_selftest(void);
#endif
