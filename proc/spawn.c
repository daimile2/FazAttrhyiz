#include "capmix/proc.h"
#include "capmix/boot.h"
#include "capmix/cap_internal.h"
#include "capmix/io.h"
#include "capmix/syscall.h"
#include "capmix/kernel.h"
#include "capmix/obs.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/linux_socket.h"
#include "capmix/linux_pipe.h"

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
            g_procs[i].fd=g_procs[i].fd_store;
            g_procs[i].linux_console=g_procs[i].linux_console_store;
            g_procs[i].linux_cloexec=g_procs[i].linux_cloexec_store;
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
    for(int i=0;i<CM_MAX_PROCS;i++)
        if(g_procs[i].used && g_procs[i].main==th && g_procs[i].elf_thread_id==th->id)return &g_procs[i];
    for (int i = 0; i < CM_MAX_PROCS; i++)
        if (g_procs[i].used && !g_procs[i].linux_group && g_procs[i].task == th->task)
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

/* Lifecycle mutation is serialized below; publication gates incomplete tasks. */
static cm_error_t spawn_elf(const cm_spawn_attr_t *attr, cm_id_t *out_pid)
{
    if (!attr->path) return CM_ERR_INVAL;
    uint64_t irq = cm_irq_save();
    int cpu=attr->cpu_set?attr->cpu:0;
    if(cpu<0 || cpu>=cm_cpu_count()) {cm_irq_restore(irq);return CM_ERR_INVAL;}
    cm_elf_image_t *im = NULL;
    cm_error_t err = cm_elf_load(attr->path, &im);
    if (err) { cm_irq_restore(irq); return err; }
    if(im->abi!=CM_ABI_LINUX && cm_cpu_id()!=0){cm_elf_destroy(im);cm_irq_restore(irq);return CM_ERR_INVAL;}
    err=cm_elf_set_args(im,attr->argv,attr->envp);
    if(err){cm_elf_destroy(im);cm_irq_restore(irq);return err;}
    cm_proc_t *p = alloc_proc();
    if (!p) { cm_elf_destroy(im); cm_irq_restore(irq); return CM_ERR_NOSPC; }
    p->pid = g_pid_next++; p->path = im->path; p->user_mode = 1;
    p->abi=im->abi;
    if(p->abi==CM_ABI_LINUX){cm_proc_t *creator=cm_proc_current();p->parent_pid=creator?creator->pid:0;}
    p->elf = im; p->aspace_pages = im->charged; p->elf_cpu = cpu;
    p->task = cm_task_create(p->path, attr->qos);
    if (!p->task || !p->task->self_obj) {
        if(p->task)memset(p->task,0,sizeof(*p->task));
        cm_elf_destroy(im); memset(p,0,sizeof(*p)); cm_irq_restore(irq);return CM_ERR_NOSPC;
    }
    p->fd[0] = 0; p->fd[1] = 1; p->fd[2] = 2;
    if(p->abi==CM_ABI_LINUX)for(unsigned i=0;i<3;i++)p->linux_console[i]=(uint8_t)(i+1);
    for(unsigned fd=0;fd<2;fd++)if(attr->stdio[fd]) {
        cm_rights_t rights=fd?CM_RIGHT_WRITE:CM_RIGHT_READ;
        /* Native bindings keep their original attenuation. Linux fork needs
         * DUP only when the trusted stdio object explicitly grants it. */
        if(p->abi==CM_ABI_LINUX)rights|=attr->stdio[fd]->default_rights&CM_RIGHT_DUPLICATE;
        int h=cm_handle_install(p->task,attr->stdio[fd],rights);
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
    if(p->abi!=CM_ABI_LINUX)__atomic_store_n(&p->published,1,__ATOMIC_RELEASE);
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
        if(p->elf)return __atomic_load_n(&p->linux_migrate,__ATOMIC_ACQUIRE)!=2 && cpu==p->elf_cpu;
        if(!cm_boot_is_selftest() && p->pid==1)return cpu==0;
    }
    return 1;
}

static cm_error_t reap_elf(cm_id_t pid)
{
    cm_proc_t *p = cm_proc_lookup(pid);
    if (!p || !p->elf || p->linux_group) return CM_ERR_INVAL;
    if (!p->exited || (p->main && p->main->id == p->elf_thread_id)) return CM_ERR_BUSY;
    for(unsigned i=0;i<CM_MAX_PROCS;i++)if(g_procs[i].used && g_procs[i].linux_group==p) {
        cm_proc_t *t=&g_procs[i];
        if(!t->linux_thread_dead || (t->main && __atomic_load_n(&t->main->id,__ATOMIC_ACQUIRE)==t->elf_thread_id))return CM_ERR_BUSY;
    }
    for(unsigned i=0;i<CM_MAX_PROCS;i++)if(g_procs[i].used && g_procs[i].linux_group==p)memset(&g_procs[i],0,sizeof(g_procs[i]));
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
    if(p->elf) {cm_error_t e=reap_elf(pid);if(e)return e;}
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
static cm_error_t release_elf(cm_id_t pid)
{
    cm_proc_t *parent=cm_proc_current();
    if((!cm_boot_is_selftest() && (!parent || parent->pid!=1)) || (parent && parent->pid==pid))return CM_ERR_DENIED;
    return release_elf_record(pid);
}
static cm_error_t release_child(cm_id_t parent,cm_id_t child)
{
    cm_proc_t *p=cm_proc_lookup(child);
    if(!p || p->parent_pid!=parent || parent==child)return CM_ERR_DENIED;
    return release_elf_record(child);
}
cm_error_t cm_proc_request_kill(cm_id_t parent,cm_id_t child)
{
    cm_proc_t *p=cm_proc_lookup(child);
    if(!p || !p->elf || p->parent_pid!=parent || child==parent || child==1)return CM_ERR_DENIED;
    if(!__atomic_load_n(&p->exited,__ATOMIC_ACQUIRE)) {
        if(p->linux_grouped)cm_linux_group_kill(p,137);
        else __atomic_store_n(&p->kill_pending,1,__ATOMIC_RELEASE);
    }
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

static cm_error_t spawn(const cm_spawn_attr_t *attr, cm_id_t *out_pid)
{
    if (!attr) return CM_ERR_INVAL;
    if (!attr->entry) return spawn_elf(attr, out_pid);
    if(cm_cpu_id()!=0)return CM_ERR_INVAL;

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

/* Linux-only fork. Children inherit fixed affinity; lifecycle mutation is serialized.
 * FILE inheritance never bypasses DUPLICATE or amplifies a right. */
static int64_t linux_fork(void)
{
    cm_proc_t *parent=cm_proc_current();
    if(!parent || parent->abi!=CM_ABI_LINUX || !parent->linux_frame)return -38;
    if(cm_linux_group(parent)->linux_grouped)return -95;
    uint64_t irq=cm_irq_save();cm_error_t e=CM_ERR_NOSPC;
    cm_proc_t *p=alloc_proc();if(!p){cm_irq_restore(irq);return -11;}
    p->abi=CM_ABI_LINUX;p->pid=g_pid_next++;p->parent_pid=parent->pid;p->linux_fork_child=1;
    e=cm_elf_clone_cow(parent->elf,&p->elf);if(e)goto fail;
    p->path=p->elf->path;p->aspace_pages=p->elf->charged;p->user_mode=1;p->elf_cpu=parent->elf_cpu;
    p->task=cm_task_create(p->path,parent->task->qos);if(!p->task || !p->task->self_obj){e=CM_ERR_NOSPC;goto fail;}
    p->linux_sigmask=parent->linux_sigmask;p->linux_sigignored=parent->linux_sigignored;
    p->stdio_mask=parent->stdio_mask;
    for(int fd=0;fd<CM_FD_MAX;fd++)if(parent->fd[fd]>=0) {
        p->linux_console[fd]=parent->linux_console[fd];p->linux_cloexec[fd]=parent->linux_cloexec[fd];
        if(p->linux_console[fd])p->fd[fd]=0;
        else {int hid;e=cm_handle_duplicate(parent->task,parent->fd[fd],p->task,&hid,~0u);if(e)goto fail;p->fd[fd]=hid;}
    }
    p->linux_regs=parent->linux_regs;p->linux_regs.rax=0;p->linux_resume=1;p->linux_fs=cm_arch_user_fs_get();
    p->main=cm_thread_create(p->task,elf_entry_wrap,p,parent->task->qos);
    if(!p->main){e=CM_ERR_NOSPC;goto fail;}
    p->elf_thread_id=p->main->id;__atomic_store_n(&p->published,1,__ATOMIC_RELEASE);
    cm_id_t pid=p->pid;kinfo("linux L2 fork: parent=%llu child=%llu pages=%u COW=RO reserves=resident cpu=%d",(unsigned long)parent->pid,(unsigned long)pid,p->elf->npages,p->elf_cpu);
    cm_irq_restore(irq);return (int64_t)pid;
fail:
    if(p->task){cm_fdio_close_all(p);if(p->task->self_obj)cm_obj_release(p->task->self_obj);memset(p->task,0,sizeof(*p->task));}
    cm_elf_destroy(p->elf);memset(p,0,sizeof(*p));cm_irq_restore(irq);
    kinfo("linux fork denied: error=%d parent=%llu",e,(unsigned long)parent->pid);
    return e==CM_ERR_DENIED?-13:e==CM_ERR_OVERCOMMIT || e==CM_ERR_NOMEM?-12:-11;
}
int64_t cm_linux_wait4(int64_t pid,uint64_t status,uint64_t options,uint64_t usage)
{
    cm_proc_t *p=cm_proc_current();if(!p)return -11;
    if(cm_linux_group(p)->linux_grouped)return -95;
    if((pid<=0 && pid!=-1) || (options&~3ull))return -22;
    if(usage)return -95;
    if(status && !cm_user_access(p->elf,status,4,1))return -14;
    for(;;) {
        int found=0;
        for(unsigned i=0;i<CM_MAX_PROCS;i++) {
            cm_proc_t *c=&g_procs[i];if(!c->used || !c->linux_fork_child || c->parent_pid!=p->pid || (pid>0 && c->pid!=(uint64_t)pid))continue;
            found=1;
            if(!__atomic_load_n(&c->exited,__ATOMIC_ACQUIRE) || (c->main && __atomic_load_n(&c->main->id,__ATOMIC_ACQUIRE)==c->elf_thread_id))continue;
            uint32_t st=((uint32_t)c->exit_code&255u)<<8;cm_id_t result=c->pid;
            if(status && cm_copyout(p->elf,status,&st,4))return -14;
            int e=cm_proc_release_child(p->pid,result);if(e==CM_ERR_BUSY){if(options&1)return 0;cm_sched_yield();cm_usermode_resume_entry();break;}if(e)return -11;
            kinfo("linux L2 wait4 reap: parent=%llu child=%llu status=%x",(unsigned long)p->pid,(unsigned long)result,st);
            return (int64_t)result;
        }
        if(!found)return -10;
        if(options&1)return 0;
        cm_sched_yield();cm_usermode_resume_entry();
        if(__atomic_load_n(&p->kill_pending,__ATOMIC_ACQUIRE))return -4;
    }
}

/* A nonblocking lifecycle gate: never spin or sleep while another CPU owns
 * create/fork/reap. No runqueue lock is held here. Callers can retry AGAIN/BUSY;
 * blocking wait4 rechecks after yielding. Includes native mutation so PID/slot
 * allocation cannot race a Linux AP. Publication still uses release/acquire. */
static unsigned lifecycle_busy;
static int lifecycle_enter(uint64_t *flags)
{
    *flags=cm_irq_save();
    if(!__atomic_exchange_n(&lifecycle_busy,1,__ATOMIC_ACQUIRE))return 1;
    cm_irq_restore(*flags);return 0;
}
static void lifecycle_leave(uint64_t flags)
{__atomic_store_n(&lifecycle_busy,0,__ATOMIC_RELEASE);cm_irq_restore(flags);}
cm_error_t cm_posix_spawn(const cm_spawn_attr_t *a,cm_id_t *pid)
{
    uint64_t f;if(!lifecycle_enter(&f))return CM_ERR_AGAIN;
    cm_id_t created=0;cm_error_t e=spawn(a,&created);
    cm_proc_t *p=e?NULL:cm_proc_lookup(created);
    if(!e && pid)*pid=created;
    /* Parent metadata is ready before AP publication. Drop the gate first so
     * a newly launched AP does not fail its first fork on its own creator. */
    __atomic_store_n(&lifecycle_busy,0,__ATOMIC_RELEASE);
    if(p && p->abi==CM_ABI_LINUX)__atomic_store_n(&p->published,1,__ATOMIC_RELEASE);
    cm_irq_restore(f);return e;
}
int64_t cm_linux_fork(void)
{uint64_t f;if(!lifecycle_enter(&f))return -11;int64_t e=linux_fork();lifecycle_leave(f);return e;}
cm_error_t cm_proc_release_elf(cm_id_t pid)
{uint64_t f;if(!lifecycle_enter(&f))return CM_ERR_BUSY;cm_error_t e=release_elf(pid);lifecycle_leave(f);return e;}
cm_error_t cm_proc_release_child(cm_id_t parent,cm_id_t child)
{uint64_t f;if(!lifecycle_enter(&f))return CM_ERR_BUSY;cm_error_t e=release_child(parent,child);lifecycle_leave(f);return e;}

cm_error_t cm_proc_reap_elf(cm_id_t pid)
{uint64_t f;if(!lifecycle_enter(&f))return CM_ERR_BUSY;cm_error_t e=reap_elf(pid);lifecycle_leave(f);return e;}

cm_proc_t *cm_linux_group(cm_proc_t *p){return p && p->linux_group?p->linux_group:p;}
void cm_linux_group_kill(cm_proc_t *p,int64_t code)
{
    cm_proc_t *g=cm_linux_group(p);if(!g)return;
    g->linux_group_exit=1;g->exit_code=code;
    for(unsigned i=0;i<CM_MAX_PROCS;i++)if(g_procs[i].used && cm_linux_group(&g_procs[i])==g && !g_procs[i].linux_thread_dead)
        __atomic_store_n(&g_procs[i].kill_pending,1,__ATOMIC_RELEASE);
    cm_linux_futex_poll();
}
/* Same-mm threads stay on their leader CPU: no new TLB-shootdown policy. */
int64_t cm_linux_clone(uint64_t flags,uint64_t stack,uint64_t ptid,uint64_t ctid,uint64_t tls)
{
    const uint64_t required=0x10f00,allowed=0x10f00|0x80000|0x100000|0x200000|0x1000000;
    if((flags&required)!=required || (flags&~allowed))return -38;
    cm_proc_t *parent=cm_proc_current(),*g=cm_linux_group(parent);
    if(!parent || !parent->linux_frame)return -38;
    /* Socket destructors may wait. Keep first thread-group teardown limited
     * to FILE/console/pipe objects, including timer-driven off-CPU exits. */
    for(unsigned fd=0;fd<CM_FD_MAX;fd++)if(cm_linux_socket_is(parent,fd))return -95;
    if(stack<16 || (stack&15) || !cm_user_access(g->elf,stack-16,16,1))return -14;
    if((flags&0x80000) && tls>=0x0000800000000000ull)return -1;
    if(((flags&0x100000) && !cm_user_access(g->elf,ptid,4,1)) ||
       ((flags&(0x200000|0x1000000)) && !cm_user_access(g->elf,ctid,4,1)))return -14;
    uint64_t f;if(!lifecycle_enter(&f))return -11;
    unsigned n=0;for(unsigned i=0;i<CM_MAX_PROCS;i++)if(g_procs[i].used && cm_linux_group(&g_procs[i])==g)n++;
    if(n>=8 || g->linux_group_exit || parent->linux_thread_dead){lifecycle_leave(f);return -11;}
    cm_proc_t *t=alloc_proc();if(!t){lifecycle_leave(f);return -11;}
    t->pid=g_pid_next++;t->abi=CM_ABI_LINUX;t->linux_group=g;t->elf=g->elf;t->task=g->task;
    t->path=g->path;t->user_mode=1;t->elf_cpu=g->elf_cpu;t->parent_pid=g->parent_pid;
    t->fd=g->fd;t->linux_console=g->linux_console;t->linux_cloexec=g->linux_cloexec;t->stdio_mask=g->stdio_mask;
    t->linux_regs=parent->linux_regs;t->linux_regs.rax=0;t->linux_regs.rsp=stack;t->linux_resume=1;
    t->linux_fs=flags&0x80000?tls:cm_arch_user_fs_get();t->linux_sigmask=parent->linux_sigmask;
    t->linux_clear_tid=flags&0x200000?ctid:0;
    t->main=cm_thread_create(g->task,elf_entry_wrap,t,g->task->qos);
    if(!t->main){memset(t,0,sizeof(*t));lifecycle_leave(f);return -11;}
    t->elf_thread_id=t->main->id;uint32_t tid=(uint32_t)t->pid;
    if(((flags&0x100000) && cm_copyout(g->elf,ptid,&tid,4)) || ((flags&0x1000000) && cm_copyout(g->elf,ctid,&tid,4))) {
        cm_thread_reap(t->main);memset(t,0,sizeof(*t));lifecycle_leave(f);return -14;
    }
    g->linux_grouped=1;__atomic_store_n(&t->published,1,__ATOMIC_RELEASE);
    kinfo("linux clone: tgid=%llu tid=%llu cpu=%d shared-mm/files entry-slot=%d",(unsigned long)g->pid,(unsigned long)t->pid,t->elf_cpu,t->elf_entry_slot);
    lifecycle_leave(f);return tid;
}
int cm_linux_thread_finish(cm_proc_t *p,int64_t code)
{
    cm_proc_t *g=cm_linux_group(p);
    if(!g)return 0;
    if(!g->linux_grouped) {
        if(p->abi==CM_ABI_LINUX && p->linux_clear_tid && !p->exited) {
            uint32_t zero=0;
            if(!cm_copyout(p->elf,p->linux_clear_tid,&zero,4))cm_linux_futex_wake(p,p->linux_clear_tid,1);
            p->linux_clear_tid=0;
        }
        return 0;
    }
    if(g->linux_cleanup)return 0;
    if(p->linux_thread_dead)return 1;
    if(__atomic_load_n(&p->kill_pending,__ATOMIC_ACQUIRE) && !g->linux_group_exit)cm_linux_group_kill(g,137);
    cm_linux_futex_cancel(p);
    if(p->linux_clear_tid){uint32_t zero=0;if(!cm_copyout(g->elf,p->linux_clear_tid,&zero,4))cm_linux_futex_wake(p,p->linux_clear_tid,1);}
    p->linux_thread_dead=1;if(p!=g){p->exit_code=code;__atomic_store_n(&p->exited,1,__ATOMIC_RELEASE);}
    else if(!g->linux_group_exit)g->exit_code=code;
    if(p->main && p->main->id==p->elf_thread_id)p->main->state=CM_TH_DEAD;
    for(unsigned i=0;i<CM_MAX_PROCS;i++)if(g_procs[i].used && cm_linux_group(&g_procs[i])==g && !g_procs[i].linux_thread_dead)return 1;
    g->linux_cleanup=1;
    if(g->main && g->main->id!=g->elf_thread_id)g->main=NULL;
    cm_proc_finish_exit(g,g->exit_code);
    return 1;
}

/* No live kernel continuation migrates. Only cm_syscall_frame may park this
 * request, once dispatch has released all resource references and gates. */
int64_t cm_linux_setaffinity(uint64_t pid,uint64_t size,uint64_t address)
{
    cm_proc_t *p=cm_proc_current();uint64_t mask;
    if(!p || !p->elf || p->abi!=CM_ABI_LINUX)return -38;
    if(pid && pid!=p->pid)return -3;
    if(size!=8)return -22;
    if(cm_copyin(p->elf,&mask,address,8))return -14;
    if(!mask || (mask&(mask-1)))return -22;
    unsigned target=0;while(!(mask&(1ull<<target)))target++;
    if(target>=(unsigned)cm_cpu_count())return -22;
    if(p->linux_grouped || p->linux_group || p->linux_fork_child || cm_proc_children_pending(p->pid))return -95;
    if(p->exited || p->kill_pending || p->linux_fd_owner || p->linux_migrate)return -11;
    /* Shared socket/pipe backend ownership is deliberately outside this MVP. */
    for(unsigned fd=0;fd<CM_FD_MAX;fd++)
        if(cm_linux_socket_is(p,fd) || cm_linux_pipe_is(p,fd))return -95;
    if((int)target==p->elf_cpu)return 0;
    if(!p->linux_frame || !p->main || !p->main->kstack)return -95;
#ifdef CAPMIX_HOSTED
    return -95; /* host model never pretends to execute a cross-CPU IRET */
#else
    p->linux_migrate_cpu=(int)target;
    __atomic_store_n(&p->linux_migrate,1,__ATOMIC_RELEASE);
    return 0;
#endif
}
int cm_proc_migrate_stopped(cm_thread_t *th)
{
    cm_proc_t *p=NULL;
    for(unsigned i=0;i<CM_MAX_PROCS;i++)
        if(g_procs[i].used && g_procs[i].main==th && g_procs[i].elf_thread_id==th->id &&
           __atomic_load_n(&g_procs[i].linux_migrate,__ATOMIC_ACQUIRE)==2){p=&g_procs[i];break;}
    if(!p || __atomic_load_n(&p->linux_migrate,__ATOMIC_ACQUIRE)!=2)return 0;
    int old=p->elf_cpu,target=p->linux_migrate_cpu;
    int rollback=p->linux_migrate_fail_once || target<0 || target>=cm_cpu_count() || p->kill_pending;
    p->linux_migrate_fail_once=0;
    /* Old scheduler owns this stopped context, holding rq lock. The new
     * landing discards the resource-free syscall tail, not a blocked syscall. */
    if(cm_arch_ctx_init(th->ctx_slot,th->entry,th->arg,th->kstack,2*CM_PAGE_SIZE))
        cm_panic("linux migrate: validated context could not be rebuilt");
    p->linux_resume=1;
    if(rollback){p->linux_regs.rax=(uint64_t)-11;p->linux_migrate_rollbacks++;}
    else {p->elf_cpu=target;p->linux_migrations++;}
    kinfo("linux migrate: pid=%llu from=%d to=%d rollback=%s off-cpu=1 FS=%llx",
          (unsigned long)p->pid,old,p->elf_cpu,rollback?"ok":"not-needed",(unsigned long)p->linux_fs);
    /* Caller drops old CPU's claim before releasing rq lock. */
    __atomic_store_n(&p->linux_migrate,0,__ATOMIC_RELEASE);
    return 1;
}
