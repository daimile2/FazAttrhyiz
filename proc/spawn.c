#include "capmix/proc.h"
#include "capmix/syscall.h"
#include "capmix/kernel.h"
#include "capmix/obs.h"

#include <string.h>

static cm_proc_t g_procs[CM_MAX_PROCS];
static cm_id_t   g_pid_next = 1;
static int       g_hello_ran;

static cm_proc_t *alloc_proc(void)
{
    for (int i = 0; i < CM_MAX_PROCS; i++)
        if (!g_procs[i].used) {
            memset(&g_procs[i], 0, sizeof(g_procs[i]));
            g_procs[i].used = 1;
            for (int f = 0; f < CM_FD_MAX; f++) g_procs[i].fd[f] = -1;
            return &g_procs[i];
        }
    return NULL;
}

void cm_proc_init(void)
{
    memset(g_procs, 0, sizeof(g_procs));
    g_pid_next = 1;
    g_hello_ran = 0;
}

cm_proc_t *cm_proc_current(void)
{
    cm_thread_t *th = cm_sched_current();
    if (!th || !th->task) return NULL;
    for (int i = 0; i < CM_MAX_PROCS; i++)
        if (g_procs[i].used && g_procs[i].task == th->task)
            return &g_procs[i];
    return NULL;
}

cm_proc_t *cm_proc_lookup(cm_id_t pid)
{
    for (int i = 0; i < CM_MAX_PROCS; i++)
        if (g_procs[i].used && g_procs[i].pid == pid)
            return &g_procs[i];
    return NULL;
}

int cm_fdio_install(cm_proc_t *p, int handle)
{
    if (!p) return -1;
    for (int i = 0; i < CM_FD_MAX; i++) {
        if (p->fd[i] < 0) {
            p->fd[i] = handle;
            return i;
        }
    }
    return -1;
}

int cm_fdio_lookup(cm_proc_t *p, int fd)
{
    if (!p || fd < 0 || fd >= CM_FD_MAX) return -1;
    return p->fd[fd];
}

static void user_entry_wrap(void *arg)
{
    cm_user_entry_t entry = (cm_user_entry_t)arg;
    if (entry) entry();
    /* If the image forgot to exit, force it. */
    cm_proc_t *p = cm_proc_current();
    if (p && !p->exited)
        cm_syscall(CM_SYS_EXIT, 0, 0, 0, 0);
}

cm_error_t cm_posix_spawn(const cm_spawn_attr_t *attr, cm_id_t *out_pid)
{
    if (!attr || !attr->entry) return CM_ERR_INVAL;

    uint64_t img = attr->image_pages ? attr->image_pages : CM_USER_IMAGE_PAGES;
    uint64_t stk = attr->stack_pages ? attr->stack_pages : CM_USER_STACK_PAGES;
    uint64_t need = img + stk;

    /* Windows no-overcommit: fail here, not later in a page fault. */
    cm_error_t ch = cm_commit_charge(need);
    if (ch != CM_OK) return ch;

    cm_proc_t *p = alloc_proc();
    if (!p) {
        cm_commit_uncharge(need);
        return CM_ERR_NOSPC;
    }

    cm_qos_t qos = attr->qos;
    p->pid = g_pid_next++;
    p->path = attr->path ? attr->path : "anon";
    p->user_mode = 1;
    p->aspace_pages = need;
    p->task = cm_task_create(p->path, qos);
    if (!p->task) {
        cm_commit_uncharge(need);
        p->used = 0;
        return CM_ERR_NOMEM;
    }

    /* Address space objects — real page tables in a later drop. */
    p->vmar = cm_obj_mint(CM_OBJ_VMAR, p, CM_RIGHT_READ | CM_RIGHT_WRITE | CM_RIGHT_MAP);
    p->vmo  = cm_obj_mint(CM_OBJ_VMO,  p, CM_RIGHT_READ | CM_RIGHT_WRITE | CM_RIGHT_MAP);
    if (p->vmar) cm_handle_install(p->task, p->vmar, p->vmar->default_rights);
    if (p->vmo)  cm_handle_install(p->task, p->vmo,  p->vmo->default_rights);

    /* fdio: stdin/stdout/stderr as slots 0/1/2 pointing at a dummy handle 0
     * (kernel console). Thick compat layer, not a native cap. */
    p->fd[0] = 0;
    p->fd[1] = 1;
    p->fd[2] = 2;

    p->main = cm_thread_create(p->task, user_entry_wrap, (void *)attr->entry, qos);
    if (!p->main) {
        cm_commit_uncharge(need);
        p->used = 0;
        return CM_ERR_NOMEM;
    }

    cm_obs_emit(cm_trace_new(), CM_EV_CAP_MINT, p->pid, need, qos);
    kinfo("spawn: pid=%llu path=%s commit=%llu pages qos=%s",
          (unsigned long)p->pid, p->path, (unsigned long)need, cm_qos_name(qos));
    if (out_pid) *out_pid = p->pid;
    return CM_OK;
}

cm_error_t cm_fork_compat(cm_id_t *out_pid)
{
    /* Conflict #2: libdispatch / capability IPC are not fork-safe.
     * We keep the number so POSIX binaries link, then refuse. */
    kwarn("fork: compat stub — use posix_spawn (race-free, one syscall)");
    if (out_pid) *out_pid = 0;
    return CM_ERR_NOTSUP;
}

int cm_proc_jetsam(void)
{
    cm_proc_t *best = NULL;
    for (int i = 0; i < CM_MAX_PROCS; i++) {
        cm_proc_t *p = &g_procs[i];
        if (!p->used || p->exited || !p->user_mode) continue;
        if (!best) { best = p; continue; }
        cm_qos_t bq = best->task ? best->task->qos : CM_QOS_DEFAULT;
        cm_qos_t pq = p->task ? p->task->qos : CM_QOS_DEFAULT;
        if (pq > bq) best = p; /* higher band number = more disposable */
    }
    if (!best) return 0;
    kinfo("jetsam: kill pid=%llu path=%s qos=%s",
          (unsigned long)best->pid, best->path,
          best->task ? cm_qos_name(best->task->qos) : "?");
    if (best->main) best->main->state = CM_TH_DEAD;
    if (best->aspace_pages) {
        cm_commit_uncharge(best->aspace_pages);
        best->aspace_pages = 0;
    }
    best->exited = 1;
    best->exit_code = 9; /* SIGKILL stand-in */
    cm_pressure_note_jetsam();
    return 1;
}

int cm_user_hello_ran(void) { return g_hello_ran; }

void cm_user_hello(void)
{
    const char *msg = "hello world from userspace";
    cm_syscall(CM_SYS_WRITE, 1, (uint64_t)(uintptr_t)msg, strlen(msg), 0);
    g_hello_ran = 1;
    uint64_t committed = 0, limit = 0;
    cm_syscall(CM_SYS_COMMIT_STAT, (uint64_t)(uintptr_t)&committed,
               (uint64_t)(uintptr_t)&limit, 0, 0);
    cm_syscall(CM_SYS_EXIT, 0, 0, 0, 0);
}
