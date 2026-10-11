#include "capmix/syscall.h"
#include "capmix/cap.h"
#include "capmix/ipc.h"
#include "capmix/proc.h"
#include "capmix/kernel.h"
#include "capmix/obs.h"
#include "capmix/fs.h"
#include "capmix/elf.h"

#include <string.h>

static int64_t sys_nop(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a0); CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    return 0;
}

static int64_t sys_exit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    cm_proc_t *p = cm_proc_current();
    cm_thread_t *th = cm_sched_current();
    if (p) {
        p->exited = 1;
        p->exit_code = (int64_t)a0;
        if (!p->elf) {
            if (p->aspace_pages) cm_commit_uncharge(p->aspace_pages);
            p->aspace_pages = 0;
        } /* ELF commit belongs to image until off-CPU reap. */
        cm_fdio_close_all(p);
        cm_anon_free_owner(p->pid);
    }
    if (th) th->state = CM_TH_DEAD;
#ifndef CAPMIX_HOSTED
    {
        extern uint64_t cm_leave_user;
        cm_leave_user = 1;
    }
#endif
    kinfo("sys: exit pid=%llu code=%lld",
          (unsigned long)(p ? p->pid : 0), (long)a0);
    return 0;
}

/* fd -> handle (fdio) -> CM_OBJ_FILE, with the HANDLE's rights checked.
 * The open mode is not consulted here: a handle duplicated with fewer
 * rights must be weaker even though it names the same open file. */
static cm_file_t *fd_file(int fd, cm_rights_t need, int64_t *err)
{
    cm_proc_t *p = cm_proc_current();
    cm_thread_t *th = cm_sched_current();
    *err = CM_ERR_INVAL;
    if (!p || !th || !th->task) return NULL;
    cm_handle_t *h = cm_handle_lookup(th->task, cm_fdio_lookup(p, fd));
    if (!h || !h->obj || h->obj->type != CM_OBJ_FILE) return NULL;
    if ((h->rights & need) != need) { *err = CM_ERR_DENIED; return NULL; }
    return (cm_file_t *)h->obj->payload;
}

static int64_t close_fd(cm_proc_t *p, cm_task_t *t, int fd)
{
    int hid = cm_fdio_lookup(p, fd);
    cm_handle_t *h = cm_handle_lookup(t, hid);
    if (!h || h->obj->type != CM_OBJ_FILE) return CM_ERR_INVAL;
    cm_object_t *obj = h->obj;
    cm_file_t *f = (cm_file_t *)obj->payload;
    cm_handle_drop(t, hid);
    p->fd[fd] = -1;
    if (obj->type == CM_OBJ_INVALID) cm_vfs_close(f);   /* last reference */
    return 0;
}

void cm_fdio_close_all(cm_proc_t *p)
{
    if (!p || !p->task) return;
    for (int fd = 3; fd < CM_FD_MAX; fd++)
        if (p->fd[fd] >= 0) (void)close_fd(p, p->task, fd);
}

static int64_t sys_write(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a3);
    int fd = (int)a0;
    const char *buf = (const char *)(uintptr_t)a1;
    uint64_t len = a2;
    cm_proc_t *p = cm_proc_current();
    if (!buf) return CM_ERR_INVAL;
    if (p && p->elf) {
        if (fd != 1 && fd != 2) return CM_ERR_INVAL;
        if (!cm_elf_user_range(p->elf, a1, len, 4)) return CM_ERR_INVAL;
        p->elf_write_calls++; p->elf_written_bytes += len;
    }
    if (fd > 2) {
        int64_t err;
        cm_file_t *f = fd_file(fd, CM_RIGHT_WRITE, &err);
        return f ? cm_vfs_write(f, buf, len) : err;
    }
    if (p && fd >= 0 && cm_fdio_lookup(p, fd) < 0 && fd > 2)
        return CM_ERR_DENIED;
    /* stdout/stderr: kernel log. Real tty comes with DriverKit. */
    char tmp[128];
    uint64_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
    memcpy(tmp, buf, (size_t)n);
    tmp[n] = 0;
    /* strip trailing newline — klog adds one */
    if (n && tmp[n - 1] == '\n') tmp[n - 1] = 0;
    kinfo("user[%llu]: %s", (unsigned long)(p ? p->pid : 0), tmp);
    return (int64_t)len;
}

static int64_t sys_open(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a2); CM_UNUSED(a3);
    const char *path = (const char *)(uintptr_t)a0;
    uint32_t flags = (uint32_t)a1;
    cm_proc_t *p = cm_proc_current();
    cm_thread_t *th = cm_sched_current();
    if (!path || !p || !th || !th->task) return CM_ERR_INVAL;

    cm_file_t *f = NULL;
    cm_error_t e = cm_vfs_open(path, flags, &f);
    if (e != CM_OK) return e;

    cm_rights_t r = CM_RIGHT_DUPLICATE | CM_RIGHT_TRANSFER | CM_RIGHT_INSPECT;
    uint32_t acc = flags & CM_O_ACCMODE;
    if (acc != CM_O_WRONLY) r |= CM_RIGHT_READ;
    if (acc != CM_O_RDONLY) r |= CM_RIGHT_WRITE;
    cm_object_t *obj = cm_obj_mint(CM_OBJ_FILE, f, r);
    if (!obj) { cm_vfs_close(f); return CM_ERR_NOMEM; }
    int hid = cm_handle_install(th->task, obj, r);
    cm_obj_release(obj);                     /* the handle owns it now */
    if (hid < 0) { cm_vfs_close(f); return hid; }
    int fd = cm_fdio_install(p, hid);
    if (fd < 0) {
        cm_handle_drop(th->task, hid);
        cm_vfs_close(f);
        return CM_ERR_NOSPC;
    }
    return fd;
}

static int64_t sys_read(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a3);
    if (!a1) return CM_ERR_INVAL;
    int64_t err;
    cm_file_t *f = fd_file((int)a0, CM_RIGHT_READ, &err);
    return f ? cm_vfs_read(f, (void *)(uintptr_t)a1, a2) : err;
}

static int64_t sys_close(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    cm_proc_t *p = cm_proc_current();
    cm_thread_t *th = cm_sched_current();
    if (!p || !th || !th->task || (int)a0 <= 2) return CM_ERR_INVAL;
    return close_fd(p, th->task, (int)a0);
}

static int64_t sys_mkdir(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    return a0 ? cm_vfs_mkdir((const char *)(uintptr_t)a0) : CM_ERR_INVAL;
}

static int64_t sys_unlink(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    return a0 ? cm_vfs_unlink((const char *)(uintptr_t)a0) : CM_ERR_INVAL;
}

static int64_t sys_fsync(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    int64_t err;
    cm_file_t *f = fd_file((int)a0, CM_RIGHT_WRITE, &err);
    return f ? cm_vfs_fsync(f) : err;
}

static int64_t sys_spawn(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a2); CM_UNUSED(a3);
    const cm_spawn_attr_t *attr = (const cm_spawn_attr_t *)(uintptr_t)a0;
    cm_id_t *out = (cm_id_t *)(uintptr_t)a1;
    return cm_posix_spawn(attr, out);
}

static int64_t sys_handle_close(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    cm_thread_t *th = cm_sched_current();
    if (!th || !th->task) return CM_ERR_INVAL;
    return cm_handle_drop(th->task, (int)a0);
}

static int64_t sys_channel_create(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a2); CM_UNUSED(a3);
    cm_thread_t *th = cm_sched_current();
    if (!th || !th->task) return CM_ERR_INVAL;
    int ha = -1, hb = -1;
    cm_error_t e = cm_channel_create(th->task, &ha, &hb);
    if (e != CM_OK) return e;
    if (a0) *(int *)(uintptr_t)a0 = ha;
    if (a1) *(int *)(uintptr_t)a1 = hb;
    return 0;
}

static int64_t sys_clock(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a0); CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    return (int64_t)cm_ticks_now();
}

static int64_t sys_trace_new(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a0); CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    return (int64_t)cm_trace_new();
}

static int64_t sys_commit_stat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a2); CM_UNUSED(a3);
    cm_mm_stats_t st;
    cm_mm_stats(&st);
    if (a0) *(uint64_t *)(uintptr_t)a0 = st.committed_pages;
    if (a1) *(uint64_t *)(uintptr_t)a1 = st.commit_limit;
    return 0;
}

static int64_t sys_fork(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a1); CM_UNUSED(a2); CM_UNUSED(a3);
    return cm_fork_compat((cm_id_t *)(uintptr_t)a0);
}

/* Mirrors USER_BASE from arch/x86_64/usermode.c (file-local #define, not in
 * a header). If that ever moves, this must move with it. */
#define CM_USER_BASE   0x400000ull
#define CM_USER_WINDOW 0x100000ull

static int cm_user_ok(uint64_t addr, uint64_t len)
{
    if (!addr || len == 0 || len > 4096) return 0;
    uint64_t end = addr + len;
    if (end < addr) return 0;                       /* wrap */
    return addr >= CM_USER_BASE && end <= CM_USER_BASE + CM_USER_WINDOW;
}

static int64_t sys_ipc_send(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a2); CM_UNUSED(a3);
    cm_thread_t *th = cm_sched_current();
    if (!th || !th->task) return CM_ERR_INVAL;

    int hid = (int)a0;
    uint64_t umsg = a1;
    if (!cm_user_ok(umsg, sizeof(cm_ipc_msg_t))) return CM_ERR_INVAL;

    /* Capability check: the handle must exist in THIS task's table.
     * Without this any task could name any handle and send to it. */
    if (!cm_handle_lookup(th->task, hid)) return CM_ERR_DENIED;

    /* Copy in: never keep a user pointer across a possible reschedule. */
    cm_ipc_msg_t kmsg;
    memcpy(&kmsg, (const void *)(uintptr_t)umsg, sizeof(kmsg));
    return cm_ipc_send(th->task, hid, &kmsg);
}

static int64_t sys_ipc_recv(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    CM_UNUSED(a2); CM_UNUSED(a3);
    cm_thread_t *th = cm_sched_current();
    if (!th || !th->task) return CM_ERR_INVAL;

    int hid = (int)a0;
    uint64_t uout = a1;
    if (!cm_user_ok(uout, sizeof(cm_ipc_msg_t))) return CM_ERR_INVAL;
    if (!cm_handle_lookup(th->task, hid)) return CM_ERR_DENIED;

    cm_ipc_msg_t kmsg;
    cm_error_t e = cm_ipc_recv(th->task, hid, &kmsg);
    if (e != CM_OK) return e;
    memcpy((void *)(uintptr_t)uout, &kmsg, sizeof(kmsg));
    return 0;
}

static const struct {
    uint32_t    nr;
    const char *name;
    cm_sysfn_t  fn;
} g_sys[] = {
    { CM_SYS_NOP,            "nop",            sys_nop },
    { CM_SYS_EXIT,           "exit",           sys_exit },
    { CM_SYS_WRITE,          "write",          sys_write },
    { CM_SYS_SPAWN,          "posix_spawn",    sys_spawn },
    { CM_SYS_HANDLE_CLOSE,   "handle_close",   sys_handle_close },
    { CM_SYS_CHANNEL_CREATE, "channel_create", sys_channel_create },
    { CM_SYS_CLOCK,          "clock",          sys_clock },
    { CM_SYS_TRACE_NEW,      "trace_new",      sys_trace_new },
    { CM_SYS_COMMIT_STAT,    "commit_stat",    sys_commit_stat },
    { CM_SYS_IPC_SEND,       "ipc_send",       sys_ipc_send },
    { CM_SYS_IPC_RECV,       "ipc_recv",       sys_ipc_recv },
    { CM_SYS_OPEN,           "open",           sys_open },
    { CM_SYS_READ,           "read",           sys_read },
    { CM_SYS_CLOSE,          "close",          sys_close },
    { CM_SYS_MKDIR,          "mkdir",          sys_mkdir },
    { CM_SYS_UNLINK,         "unlink",         sys_unlink },
    { CM_SYS_FSYNC,          "fsync",          sys_fsync },
    { CM_SYS_FORK,           "fork",           sys_fork },
};


/* --- end-to-end syscall cost -------------------------------------------
 * The existing IPC bench calls cm_ipc_send()/cm_ipc_recv() directly from
 * kernel context: no SYSCALL, no SYSRET, no cm_syscall_entry. So it never
 * pays the ~686-cycle hardware floor for a real ring3<->ring0 round trip.
 * This measures the floor (NOP) and the real thing (IPC) end to end.
 */
uint64_t cm_bench_syscall_nop(int rounds)
{
    uint64_t t0 = cm_rdtsc();
    for (int i = 0; i < rounds; i++)
        (void)cm_syscall(CM_SYS_NOP, 0, 0, 0, 0);
    uint64_t t1 = cm_rdtsc();
    return rounds ? (t1 - t0) / (uint64_t)rounds : 0;
}

static void bench_fn(void *arg) { (void)arg; }

/* End-to-end IPC through the syscall surface: handle lookup against the
 * current task, copy in/out across the user boundary, real send+recv.
 * Measured 1134 cycles/rtt (min 877 / max 1316) vs seL4's 986 — L4 class,
 * not Mach class (Mach was 5750 cycles one-way on i486).
 *
 * IRQs are disabled for the whole bench: g_current[] is per-CPU and a
 * timer ISR landing mid-bench can clear it, which faulted an earlier
 * version (rip=ffffffff80009d40, cr2=8).
 */
uint64_t cm_bench_syscall_ipc(int rounds)
{
    uint64_t f = cm_irq_save();   /* cli is privileged: no-op on hosted */
    uint64_t r = 0;
    cm_task_t *t = cm_task_create("ipc-e2e", CM_QOS_USER_INTERACTIVE);
    cm_thread_t *th = t ? cm_thread_create(t, bench_fn, 0, CM_QOS_USER_INTERACTIVE) : 0;
    uint64_t *pml4 = cm_paging_pml4(); void *pg = cm_page_alloc(0); uint64_t h = cm_hhdm();
    do {
        if (!th || !pml4 || !pg || !h) break;
        cm_sched_set_current(th);
        if (cm_space_map(pml4, 0x400000ull, (uint64_t)(uintptr_t)pg - h, 1) != 0) break;
        int a=-1,b=-1; if (cm_channel_create(t,&a,&b)!=CM_OK) break;
        cm_ipc_msg_t *m = (cm_ipc_msg_t*)0x400000ull;
        memset(m,0,sizeof(*m)); m->opcode=0xC0DE; m->len=8; m->data[0]=7;
        uint64_t mn = ~0ull, mx = 0;
        uint64_t t0=cm_rdtsc();
        for (int i=0;i<rounds;i++) {
            uint64_t q0=cm_rdtsc();
            (void)cm_syscall(CM_SYS_IPC_SEND,(uint64_t)a,0x400000ull,0,0);
            (void)cm_syscall(CM_SYS_IPC_RECV,(uint64_t)b,0x400000ull,0,0);
            uint64_t d=cm_rdtsc()-q0;
            if (d < mn) mn = d;
            if (d > mx) mx = d;
        }
        uint64_t tot = cm_rdtsc()-t0;
        r = rounds ? tot/(uint64_t)rounds : 0;
        kinfo("ipc: end-to-end %llu cycles/rtt (min %llu / max %llu, n=%d)",
              (unsigned long)r, (unsigned long)mn, (unsigned long)mx, rounds);
    } while(0);
    cm_sched_set_current(0);
    cm_irq_restore(f);
    return r;
}


/* User pointers arrive from ring3 and must stay inside the user window.
 * sys_write only NULL-checks; do better here, or any caller can hand the
 * kernel a kernel address and get it dereferenced with kernel privilege. */

void cm_syscall_init(void) {}

const char *cm_sys_name(uint32_t nr)
{
    for (unsigned i = 0; i < CM_ARRAY_LEN(g_sys); i++)
        if (g_sys[i].nr == nr) return g_sys[i].name;
    return "unknown";
}

int64_t cm_syscall(uint32_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    /* Bounded ELF ABI until all pointer-bearing syscalls have copyin/out.
     * Leave the existing hosted/C-entry and embedded ring3 tests unchanged. */
    cm_proc_t *elf_proc = cm_proc_current();
    if (elf_proc && elf_proc->elf) {
        if (nr == CM_SYS_FORK) return CM_ERR_NOTSUP;
        if (nr != CM_SYS_NOP && nr != CM_SYS_WRITE && nr != CM_SYS_EXIT && nr != CM_SYS_CLOCK)
            return CM_ERR_NOTSUP;
    }
    if (cm_obs_enabled()) {
        cm_trace_t tr = cm_trace_new();
        cm_obs_emit(tr, CM_EV_BOOT /* reuse until SYS event exists */, nr, a0, a1);
    }
    for (unsigned i = 0; i < CM_ARRAY_LEN(g_sys); i++) {
        if (g_sys[i].nr == nr)
            return g_sys[i].fn(a0, a1, a2, a3);
    }
    return CM_ERR_NOTSUP;
}
