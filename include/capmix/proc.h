#ifndef CAPMIX_PROC_H
#define CAPMIX_PROC_H

#include "types.h"
#include "abi.h"
#include "linux_process.h"
#include "sched.h"

/* User process + posix_spawn.
 * Conflict #2: fork is not first-class. Spawn is one syscall, race-free.
 */

#define CM_MAX_PROCS     32
#define CM_FD_MAX        32
#define CM_USER_STACK_PAGES  4
#define CM_USER_IMAGE_PAGES  8

typedef void (*cm_user_entry_t)(void);

typedef struct {
    const char      *path;
    cm_user_entry_t  entry;
    cm_qos_t         qos;
    uint64_t         image_pages;
    uint64_t         stack_pages;
    const char *const *argv; /* trusted native vectors; copied before ELF publication */
    const char *const *envp;
    int cpu_set, cpu; /* optional fixed ELF affinity; default BSP */
    cm_object_t *stdio[2]; /* trusted retained FILE objects, installed before publish */
} cm_spawn_attr_t;

struct cm_elf_image;

typedef struct cm_proc {
    int           used;
    cm_abi_t abi;
    uint64_t linux_enosys,linux_write_bytes,linux_last_syscall,linux_calls;
    cm_linux_regs_t linux_regs;
    cm_linux_regs_t *linux_frame; /* valid only while in the hardware syscall */
    struct cm_proc *linux_group; /* NULL on leader; shared mm/task/files */
    uint64_t linux_clear_tid;
    int linux_grouped,linux_thread_dead,linux_cleanup,linux_group_exit;
    uint64_t linux_fd_owner; /* leader: nonblocking syscall gate, owner TID */
    unsigned linux_fd_depth; /* recursive writev -> write by the same owner */
    uint64_t linux_fs;
    uint64_t linux_sigmask,linux_sigignored; /* single-thread metadata; no delivery */
    int linux_resume,linux_fork_child;
    /* 0 idle, 1 requested, 2 off-user-stack handoff; rq lock publishes CPU. */
    int linux_migrate,linux_migrate_cpu,linux_migrate_fail_once;
    unsigned linux_migrations,linux_migrate_rollbacks;
    uint8_t linux_console_store[CM_FD_MAX],linux_cloexec_store[CM_FD_MAX];
    uint8_t *linux_console,*linux_cloexec;
    cm_id_t       pid;
    cm_task_t    *task;
    cm_thread_t  *main;
    const char   *path;
    int           user_mode;
    int           exited;
    int64_t       exit_code;
    uint64_t      aspace_pages;
    int fd_store[CM_FD_MAX];
    int *fd; /* posix fd → capability handle, -1 empty */
    unsigned stdio_mask; /* bound fd0/1; a closed binding never falls back to console */
    cm_object_t  *vmar;
    cm_object_t  *vmo;
    struct cm_elf_image *elf;
    cm_id_t       elf_thread_id;
    int           elf_started, elf_cpl, elf_write_calls;
    uint64_t      elf_written_bytes, elf_timer_entries, elf_timer_before;
    uint64_t      elf_preempts, elf_run_ticks;
    int           elf_cpu, elf_entry_slot;
    uint64_t elf_start_tsc, elf_end_tsc;
    int published;
    int kill_pending, syscall_depth; /* atomic: terminate only outside syscall frames */
    cm_id_t parent_pid;
} cm_proc_t;

cm_proc_t *cm_linux_group(cm_proc_t *);
int cm_linux_thread_finish(cm_proc_t *,int64_t);
void cm_linux_group_kill(cm_proc_t *,int64_t);
int64_t cm_linux_clone(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
void cm_linux_futex_poll(void);
void cm_linux_futex_cancel(cm_proc_t *);
int64_t cm_linux_futex(uint64_t,uint64_t,uint64_t,uint64_t);
int64_t cm_linux_futex_wake(cm_proc_t *,uint64_t,unsigned);
int cm_linux_threads_selftest(void);
void       cm_proc_init(void);
cm_proc_t *cm_proc_current(void);
cm_proc_t *cm_proc_lookup(cm_id_t pid);

/* entry!=NULL: existing C entry; entry==NULL: load static ET_EXEC from path. */
cm_error_t cm_posix_spawn(const cm_spawn_attr_t *attr, cm_id_t *out_pid);

/* Release an exited ELF image only after its thread has left the CPU. */
cm_error_t cm_proc_reap_elf(cm_id_t pid);
/* Product PID1 only: release an off-CPU ELF exit record/task after image reap. */
cm_error_t cm_proc_release_product_elf(cm_id_t pid);
/* BSP selftest or product PID1; recycle record/task only after off-CPU reap. */
cm_error_t cm_proc_release_elf(cm_id_t pid);
cm_error_t cm_proc_release_child(cm_id_t parent,cm_id_t child);
int cm_proc_children_pending(cm_id_t parent);
cm_error_t cm_proc_request_kill(cm_id_t parent, cm_id_t child);
void cm_proc_finish_exit(cm_proc_t *p, int64_t code);
int cm_proc_reap_orphans(void);
int64_t cm_linux_setaffinity(uint64_t pid,uint64_t size,uint64_t mask);
/* Called with rq lock, after swap to the OLD CPU scheduler. */
int cm_proc_migrate_stopped(cm_thread_t *th);
/* ELF placement changes only at an explicit, off-CPU syscall handoff. */
int cm_proc_task_is_elf(const cm_task_t *task);
int cm_proc_task_cpu_allowed(const cm_task_t *task, int cpu);

/* Compat only. Hosted: returns -CM_ERR_NOTSUP and logs the reason. */
cm_error_t cm_fork_compat(cm_id_t *out_pid);

int        cm_fdio_install(cm_proc_t *p, int handle);
int        cm_fdio_lookup(cm_proc_t *p, int fd);
void       cm_fdio_close_all(cm_proc_t *p);

int        cm_user_hello_ran(void);
void       cm_user_hello(void);
cm_id_t    cm_proc_jetsam_pick(int *is_oom); /* L4: kill one, return pid */
int        cm_proc_alive(cm_id_t pid);
int        cm_proc_kill_log(cm_id_t *out, int max);
void       cm_proc_kill_log_reset(void);

#endif
