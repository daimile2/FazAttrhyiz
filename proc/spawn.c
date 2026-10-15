#include "capmix/proc.h"
#include "capmix/boot.h"
#include "capmix/cap_internal.h"
#include "capmix/io.h"
#include "capmix/syscall.h"
#include "capmix/kernel.h"
#include "capmix/obs.h"
#include "capmix/elf.h"

#include <string.h>
#include <stdint.h>

static cm_proc_t g_procs[CM_MAX_PROCS];
static cm_id_t   g_pid_next = 1;
static int       g_hello_ran;


static cm_proc_t *alloc_proc(void)
{
    for (int i = 0; i < CM_MAX_PROCS; i++)
        if (!g_procs[i].used) {
            memset(&g_procs[i], 0, sizeof(g_procs[i]));
            g_procs[i].used = 1;
            g_procs[i].elf_entry_slot=i;
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

static void elf_entry_wrap(void *arg)
{
    cm_proc_t *p = arg;
    if(__atomic_load_n(&p->kill_pending,__ATOMIC_ACQUIRE)) {cm_proc_finish_exit(p,137);return;}
    cm_usermode_run_elf(p);
}

/* Creation is owned by BSP; publication gates AP picks until metadata is complete. */
static cm_error_t spawn_elf(const cm_spawn_attr_t *attr, cm_id_t *out_pid)
{
    if (!attr->path || cm_cpu_id() != 0) return CM_ERR_INVAL;
    uint64_t irq = cm_irq_save();
    int cpu=attr->cpu_set?attr->cpu:0;
    if(cpu<0 || cpu>=cm_cpu_count()) {cm_irq_restore(irq);return CM_ERR_INVAL;}
    cm_elf_image_t *im = NULL;
    cm_error_t err = cm_elf_load(attr->path, &im);
    if (err) { cm_irq_restore(irq); return err; }
    err=cm_elf_set_args(im,attr->argv,attr->envp);
    if(err){cm_elf_destroy(im);cm_irq_restore(irq);return err;}
    cm_proc_t *p = alloc_proc();
    if (!p) { cm_elf_destroy(im); cm_irq_restore(irq); return CM_ERR_NOSPC; }
    p->pid = g_pid_next++; p->path = im->path; p->user_mode = 1;
    p->abi=im->abi;
    p->elf = im; p->aspace_pages = im->charged; p->elf_cpu = cpu;
    p->task = cm_task_create(p->path, attr->qos);
    if (!p->task || !p->task->self_obj) {
        if(p->task)memset(p->task,0,sizeof(*p->task));
        cm_elf_destroy(im); memset(p,0,sizeof(*p)); cm_irq_restore(irq);return CM_ERR_NOSPC;
    }
    p->fd[0] = 0; p->fd[1] = 1; p->fd[2] = 2;
    if(p->abi==CM_ABI_LINUX)for(unsigned i=0;i<3;i++)p->linux_console[i]=(uint8_t)(i+1);
    for(unsigned fd=0;fd<2;fd++)if(attr->stdio[fd]) {
        int h=cm_handle_install(p->task,attr->stdio[fd],fd?CM_RIGHT_WRITE:CM_RIGHT_READ);
        if(h<0) {
            cm_fdio_close_all(p);cm_obj_release(p->task->self_obj);
            memset(p->task,0,sizeof(*p->task));cm_elf_destroy(im);memset(p,0,sizeof(*p));
            cm_irq_restore(irq);return h;
        }
        p->fd[fd]=h;p->stdio_mask|=1u<<fd;p->linux_console[fd]=0;
    }
    /* The affinity gate excludes this task until main/id are installed. */
    p->main = cm_thread_create(p->task, elf_entry_wrap, p, attr->qos);
    if (!p->main) {
        /* This task has not been published to any caller and has no thread. */
        cm_fdio_close_all(p);cm_obj_release(p->task->self_obj);
        memset(p->task, 0, sizeof(*p->task));
        cm_elf_destroy(im); p->elf = NULL; p->path = NULL; p->used = 0;
        cm_irq_restore(irq); return CM_ERR_NOSPC;
    }
    p->elf_thread_id = p->main->id;
    __atomic_store_n(&p->published,1,__ATOMIC_RELEASE);
    kinfo("exec spawn: pid=%llu path=%s ELF entry=%llx full=%llx shadow=%llx",
          (unsigned long)p->pid, p->path, (unsigned long)im->entry,
          (unsigned long)im->cr3, (unsigned long)im->shadow_cr3);
    if (out_pid) *out_pid = p->pid;
    cm_irq_restore(irq);
    return CM_OK;
}

int cm_proc_task_is_elf(const cm_task_t *task)
{
    for(int i=0;i<CM_MAX_PROCS;i++)
        if(g_procs[i].used && g_procs[i].task==task) return g_procs[i].elf!=NULL;
    return 0;
}
int cm_proc_task_cpu_allowed(const cm_task_t *task,int cpu)
{
    for(int i=0;i<CM_MAX_PROCS;i++) {
        cm_proc_t *p=&g_procs[i];
        if(!p->used || p->task!=task)continue;
        if(!__atomic_load_n(&p->published,__ATOMIC_ACQUIRE))return 0;
        if(p->elf)return cpu==p->elf_cpu;
        if(!cm_boot_is_selftest() && p->pid==1)return cpu==0;
    }
    return 1;
}

cm_error_t cm_proc_reap_elf(cm_id_t pid)
{
    cm_proc_t *p = cm_proc_lookup(pid);
    if (!p || !p->elf) return CM_ERR_INVAL;
    if (!p->exited || (p->main && p->main->id == p->elf_thread_id)) return CM_ERR_BUSY;
    cm_elf_image_t *im = p->elf;
    p->task->name = "elf-exited"; /* do not retain a name in freed image metadata */
    p->elf = NULL; p->path = "elf-exited"; p->aspace_pages = 0; p->main = NULL;
    cm_elf_destroy(im);
    kinfo("exec reap: pid=%llu image/pages/commit reclaimed entry-stack inactive", (unsigned long)pid);
    return CM_OK;
}

static cm_error_t release_elf_record(cm_id_t pid)
{
    cm_proc_t *p=cm_proc_lookup(pid);
    if(!p || !p->exited || !p->elf_thread_id)return CM_ERR_INVAL;
    if(p->elf) {cm_error_t e=cm_proc_reap_elf(pid);if(e)return e;}
    if(p->main)return CM_ERR_BUSY;
    cm_task_t *task=p->task;
    for(int h=1;h<CM_HANDLES_PER_TASK;h++)if(cm_handle_lookup(task,h))cm_handle_drop(task,h);
    uint64_t flags=cm_cap_lock();
    int exclusive=task->self_obj && task->self_obj->refs==1;
    cm_cap_unlock(flags);
    if(!exclusive)return CM_ERR_BUSY; /* do not invalidate an externally held task */
    cm_obj_release(task->self_obj);
    memset(task,0,sizeof(*task));memset(p,0,sizeof(*p));
    kinfo("exec record reap: pid=%llu task/entry-stack slot recycled",(unsigned long)pid);
    return CM_OK;
}
cm_error_t cm_proc_release_elf(cm_id_t pid)
{
    cm_proc_t *parent=cm_proc_current();
    if(cm_cpu_id()!=0 || (!cm_boot_is_selftest() && (!parent || parent->pid!=1)) || (parent && parent->pid==pid))return CM_ERR_DENIED;
    return release_elf_record(pid);
}
cm_error_t cm_proc_release_child(cm_id_t parent,cm_id_t child)
{
    cm_proc_t *p=cm_proc_lookup(child);
    if(cm_cpu_id()!=0 || !p || p->parent_pid!=parent || parent==child)return CM_ERR_DENIED;
    return release_elf_record(child);
}
cm_error_t cm_proc_request_kill(cm_id_t parent,cm_id_t child)
{
    cm_proc_t *p=cm_proc_lookup(child);
    if(!p || !p->elf || p->parent_pid!=parent || child==parent || child==1)return CM_ERR_DENIED;
    if(!__atomic_load_n(&p->exited,__ATOMIC_ACQUIRE))
        __atomic_store_n(&p->kill_pending,1,__ATOMIC_RELEASE);
    kinfo("user kill requested: parent=%llu pid=%llu exit=137",(unsigned long)parent,(unsigned long)child);
    return CM_OK;
}
int cm_proc_children_pending(cm_id_t parent)
{
    for(unsigned i=0;i<CM_MAX_PROCS;i++)if(g_procs[i].used && g_procs[i].parent_pid==parent && g_procs[i].elf)return 1;
    return 0;
}
int cm_proc_reap_orphans(void)
{
    int pending=0;
    for(unsigned i=0;i<CM_MAX_PROCS;i++) {
        cm_proc_t *p=&g_procs[i];if(!p->used || !p->elf || !p->parent_pid)continue;
        cm_proc_t *parent=cm_proc_lookup(p->parent_pid);
        if(parent && !__atomic_load_n(&parent->exited,__ATOMIC_ACQUIRE))continue;
        if(__atomic_load_n(&p->exited,__ATOMIC_ACQUIRE) && (!p->main || __atomic_load_n(&p->main->id,__ATOMIC_ACQUIRE)!=p->elf_thread_id)) {
            cm_id_t pid=p->pid;
            if(cm_proc_release_child(p->parent_pid,pid)==CM_OK){kinfo("user orphan reap: pid=%llu",(unsigned long)pid);continue;}
        }
        pending++;
    }
    return pending;
}
cm_error_t cm_proc_release_product_elf(cm_id_t pid)
{
    if(cm_boot_is_selftest())return CM_ERR_DENIED;
    return cm_proc_release_elf(pid);
}

cm_error_t cm_posix_spawn(const cm_spawn_attr_t *attr, cm_id_t *out_pid)
{
    if (!attr || cm_cpu_id()!=0) return CM_ERR_INVAL;
    if (!attr->entry) return spawn_elf(attr, out_pid);

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

    __atomic_store_n(&p->published,1,__ATOMIC_RELEASE);
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

int cm_proc_alive(cm_id_t pid)
{
    cm_proc_t *p = cm_proc_lookup(pid);
    return !p || !p->exited;     /* kernel-owned (no proc) counts as alive */
}

/* Victims in the order L4 took them, for the selftest and post-mortem. */
#define KILL_LOG_N 16
static cm_id_t g_kill_log[KILL_LOG_N];
static int     g_kill_n;

int cm_proc_kill_log(cm_id_t *out, int max)
{
    int n = g_kill_n < max ? g_kill_n : max;
    for (int i = 0; i < n; i++) out[i] = g_kill_log[i];
    return n;
}

void cm_proc_kill_log_reset(void) { g_kill_n = 0; }

static void proc_kill(cm_proc_t *p, int64_t code)
{
    if (g_kill_n < KILL_LOG_N) g_kill_log[g_kill_n++] = p->pid;
    cm_fdio_close_all(p);
    if (p->main) p->main->state = CM_TH_DEAD;
    if (p->aspace_pages && !p->elf) {
        cm_commit_uncharge(p->aspace_pages);
        p->aspace_pages = 0;
    }
    p->exited = 1;
    p->exit_code = code;
    cm_drv_process_exit(p->pid);
}

/* L4 victim selection.
 *
 * jetsam (macOS): walk bands from most to least disposable —
 *   Idle -> Background -> Utility -> Default -> UserInitiated
 * and take the biggest process in the first non-empty band. UserInteractive
 * (foreground) is never a jetsam victim: killing what the user is looking
 * at to save memory is the wrong trade.
 *
 * OOM killer (Linux), only when no band candidate exists: badness =
 * footprint in permille of RAM + oom_score_adj, foreground included.
 *
 * Processes holding no anon memory are skipped: killing them frees nothing.
 * Returns the pid (the caller releases its memory) or 0. */
cm_id_t cm_proc_jetsam_pick(int *is_oom)
{
    cm_proc_t *best = NULL;
    uint64_t best_rss = 0;
    int best_band = -1;
    for (int i = 0; i < CM_MAX_PROCS; i++) {
        cm_proc_t *p = &g_procs[i];
        if (!p->used || p->exited || !p->user_mode || !p->task) continue;
        int band = (int)p->task->qos;
        if (band == CM_QOS_USER_INTERACTIVE) continue;
        uint64_t rss = cm_anon_owner_pages(p->pid);
        if (!rss) continue;
        int64_t adj = p->task->oom_score_adj;
        int64_t score = (int64_t)rss + adj;
        int64_t bscore = best ? (int64_t)best_rss + best->task->oom_score_adj : 0;
        if (band > best_band || (band == best_band && score > bscore)) {
            best = p; best_rss = rss; best_band = band;
        }
    }
    if (is_oom) *is_oom = 0;
    if (!best) {
        int64_t bad_best = INT64_MIN;
        uint64_t total = cm_mm_total_pages() ? cm_mm_total_pages() : 1;
        for (int i = 0; i < CM_MAX_PROCS; i++) {
            cm_proc_t *p = &g_procs[i];
            if (!p->used || p->exited || !p->user_mode || !p->task) continue;
            uint64_t rss = cm_anon_owner_pages(p->pid);
            if (!rss) continue;
            int64_t bad = (int64_t)(rss * 1000 / total) + p->task->oom_score_adj;
            if (bad > bad_best) { bad_best = bad; best = p; best_rss = rss; }
        }
        if (!best) return 0;
        if (is_oom) *is_oom = 1;
    }
    kinfo("%s: kill pid=%llu path=%s band=%s rss=%llu pages",
          (is_oom && *is_oom) ? "oom" : "jetsam",
          (unsigned long)best->pid, best->path,
          cm_qos_name(best->task->qos), (unsigned long)best_rss);
    proc_kill(best, 9);   /* SIGKILL stand-in */
    return best->pid;
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
