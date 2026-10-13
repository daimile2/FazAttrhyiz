#ifndef CAPMIX_PROC_H
#define CAPMIX_PROC_H

#include "types.h"
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
} cm_spawn_attr_t;

struct cm_elf_image;

typedef struct cm_proc {
    int           used;
    cm_id_t       pid;
    cm_task_t    *task;
    cm_thread_t  *main;
    const char   *path;
    int           user_mode;
    int           exited;
    int64_t       exit_code;
    uint64_t      aspace_pages;
    int           fd[CM_FD_MAX]; /* posix fd → capability handle, -1 empty */
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
    cm_id_t parent_pid;
} cm_proc_t;

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
int cm_proc_reap_orphans(void);
/* Each ELF has fixed CPU affinity, including blocked/exit cleanup. */
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
