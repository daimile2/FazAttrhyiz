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
} cm_spawn_attr_t;

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
} cm_proc_t;

void       cm_proc_init(void);
cm_proc_t *cm_proc_current(void);
cm_proc_t *cm_proc_lookup(cm_id_t pid);

/* First-class. Charges commit up front (no-overcommit). */
cm_error_t cm_posix_spawn(const cm_spawn_attr_t *attr, cm_id_t *out_pid);

/* Compat only. Hosted: returns -CM_ERR_NOTSUP and logs the reason. */
cm_error_t cm_fork_compat(cm_id_t *out_pid);

int        cm_fdio_install(cm_proc_t *p, int handle);
int        cm_fdio_lookup(cm_proc_t *p, int fd);

int        cm_user_hello_ran(void);
void       cm_user_hello(void);
int        cm_proc_jetsam(void); /* kill lowest QoS live user proc */

#endif
