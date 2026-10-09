#include "capmix/fs.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

/* VFS: mount table + path walk + open-file table, over cm_fs_ops_t.
 *
 * Files and network stay in the kernel (XNU lesson, conflict #1): this is a
 * function call into the FS, never an IPC to a filesystem server. The POSIX
 * fd surface is a thin layer ABOVE it (proc/syscall.c turns a cm_file_t
 * into a CM_OBJ_FILE capability, fdio maps fd -> handle), never unified
 * with the capability primitive itself (conflict #3). */

extern cm_error_t        cm_cowfs_init(void);
extern const cm_fs_ops_t *cm_cowfs_ops(void);
extern cm_error_t        cm_cowfs_clone_ino(cm_id_t src, cm_id_t dstdir, const char *name,
                                            size_t nlen, cm_id_t *out);
extern uint32_t          cm_cowfs_bmap_ino(cm_id_t ino, uint64_t off);

#define VFS_MAX_MOUNTS 16
#define VFS_MAX_FILES  64
#define VFS_PATH_MAX   128

typedef struct {
    int                used;
    char               at[32];
    size_t             atlen;
    const cm_fs_ops_t *ops;
    void              *inst;
} vfs_mount_t;

struct cm_file {
    int      used;
    int      mnt;
    cm_id_t  ino;
    uint64_t off;
    uint32_t mode;     /* CM_O_* access mode + APPEND */
};

static vfs_mount_t g_mnt[VFS_MAX_MOUNTS];
static cm_file_t   g_files[VFS_MAX_FILES];

/* Recursive lock: cowfs re-enters cm_vfs_mount() while a snapshot call
 * holds it. Owner is the CPU, so an ISR on another CPU cannot slip in. */
static volatile int g_vfs_owner;     /* cpu + 1, 0 = free */
static int          g_vfs_depth;
static uint64_t     g_vfs_flags;

void cm_vfs_lock(void)
{
    uint64_t f = cm_irq_save();
    int me = cm_cpu_id() + 1;
    if (g_vfs_owner == me) { g_vfs_depth++; cm_irq_restore(f); return; }
    while (!__sync_bool_compare_and_swap(&g_vfs_owner, 0, me))
        __asm__ volatile("pause" ::: "memory");
    g_vfs_depth = 1;
    g_vfs_flags = f;
}

void cm_vfs_unlock(void)
{
    if (--g_vfs_depth > 0) return;
    uint64_t f = g_vfs_flags;
    __sync_lock_release(&g_vfs_owner);
    cm_irq_restore(f);
}

/* ---- mounts ----------------------------------------------------------- */

cm_error_t cm_vfs_mount(const char *at, const cm_fs_ops_t *ops, void *inst)
{
    size_t n = at ? strlen(at) : 0;
    if (!n || n >= sizeof(g_mnt[0].at) || at[0] != '/' || !ops) return CM_ERR_INVAL;
    cm_vfs_lock();
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mnt[i].used && g_mnt[i].atlen == n && memcmp(g_mnt[i].at, at, n) == 0) {
            cm_vfs_unlock();
            return CM_ERR_EXIST;
        }
        if (!g_mnt[i].used && slot < 0) slot = i;
    }
    if (slot < 0) { cm_vfs_unlock(); return CM_ERR_NOSPC; }
    memcpy(g_mnt[slot].at, at, n + 1);
    g_mnt[slot].atlen = n;
    g_mnt[slot].ops = ops;
    g_mnt[slot].inst = inst;
    g_mnt[slot].used = 1;
    cm_vfs_unlock();
    return CM_OK;
}

cm_error_t cm_vfs_umount(const char *at)
{
    size_t n = at ? strlen(at) : 0;
    cm_vfs_lock();
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mnt[i].used || g_mnt[i].atlen != n || memcmp(g_mnt[i].at, at, n)) continue;
        for (int f = 0; f < VFS_MAX_FILES; f++)
            if (g_files[f].used && g_files[f].mnt == i) {
                cm_vfs_unlock();
                return CM_ERR_BUSY;
            }
        g_mnt[i].used = 0;
        cm_vfs_unlock();
        return CM_OK;
    }
    cm_vfs_unlock();
    return CM_ERR_NOENT;
}

/* Longest mount prefix that ends on a component boundary. */
static int find_mount(const char *path, const char **rest)
{
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mnt[i].used) continue;
        size_t n = g_mnt[i].atlen;
        if (n == 1) {                              /* "/" matches everything */
            if (best < 0) { best = i; bl = 0; }
            continue;
        }
        if (memcmp(path, g_mnt[i].at, n) == 0 && (path[n] == 0 || path[n] == '/') && n > bl) {
            best = i;
            bl = n;
        }
    }
    if (rest) *rest = path + bl;
    return best;
}

/* Walk `path`. With parent=1 stop one short and hand back the last name. */
static cm_error_t resolve(const char *path, int parent, int *out_mnt, cm_id_t *out_ino,
                          const char **leaf, size_t *leaflen)
{
    if (!path || path[0] != '/' || strlen(path) >= VFS_PATH_MAX) return CM_ERR_INVAL;
    const char *p;
    int m = find_mount(path, &p);
    if (m < 0) return CM_ERR_NOENT;
    const cm_fs_ops_t *ops = g_mnt[m].ops;
    cm_id_t cur = ops->root(g_mnt[m].inst);

    for (;;) {
        while (*p == '/') p++;
        if (!*p) {
            if (parent) return CM_ERR_INVAL;       /* no leaf to create */
            break;
        }
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - s);
        if (n > CM_FS_NAME_MAX) return CM_ERR_INVAL;
        const char *q = p;
        while (*q == '/') q++;
        if (parent && !*q) {
            if (leaf) *leaf = s;
            if (leaflen) *leaflen = n;
            break;
        }
        cm_id_t next = 0;
        cm_error_t e = ops->lookup(g_mnt[m].inst, cur, s, n, &next);
        if (e != CM_OK) return e;
        cur = next;
    }
    *out_mnt = m;
    *out_ino = cur;
    return CM_OK;
}

/* ---- files ------------------------------------------------------------ */

cm_error_t cm_vfs_open(const char *path, uint32_t oflags, cm_file_t **out)
{
    if (!out) return CM_ERR_INVAL;
    cm_vfs_lock();
    int m = -1;
    cm_id_t ino = 0;
    cm_error_t e = resolve(path, 0, &m, &ino, NULL, NULL);
    if (e == CM_ERR_NOENT && (oflags & CM_O_CREAT)) {
        const char *leaf = NULL;
        size_t ll = 0;
        cm_id_t dir = 0;
        e = resolve(path, 1, &m, &dir, &leaf, &ll);
        if (e == CM_OK)
            e = g_mnt[m].ops->create(g_mnt[m].inst, dir, leaf, ll, CM_INO_FILE,
                                     (oflags & CM_O_NOCOW) ? CM_FS_NOCOW : 0, &ino);
    }
    if (e != CM_OK) { cm_vfs_unlock(); return e; }

    cm_stat_t st;
    if ((e = g_mnt[m].ops->getattr(g_mnt[m].inst, ino, &st)) != CM_OK) {
        cm_vfs_unlock();
        return e;
    }
    uint32_t acc = oflags & CM_O_ACCMODE;
    if (st.kind == CM_INO_DIR && acc != CM_O_RDONLY) { cm_vfs_unlock(); return CM_ERR_ISDIR; }
    if ((oflags & CM_O_TRUNC) && acc != CM_O_RDONLY) {
        if ((e = g_mnt[m].ops->truncate(g_mnt[m].inst, ino, 0)) != CM_OK) {
            cm_vfs_unlock();
            return e;
        }
    }
    cm_file_t *f = NULL;
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (!g_files[i].used) { f = &g_files[i]; break; }
    if (!f) { cm_vfs_unlock(); return CM_ERR_NOSPC; }
    f->used = 1;
    f->mnt = m;
    f->ino = ino;
    f->off = 0;
    f->mode = oflags & (CM_O_ACCMODE | CM_O_APPEND);
    *out = f;
    cm_vfs_unlock();
    return CM_OK;
}

uint32_t cm_vfs_file_mode(const cm_file_t *f) { return f ? f->mode : 0; }

static int can_read(const cm_file_t *f)  { return (f->mode & CM_O_ACCMODE) != CM_O_WRONLY; }
static int can_write(const cm_file_t *f) { return (f->mode & CM_O_ACCMODE) != CM_O_RDONLY; }

int64_t cm_vfs_pread(cm_file_t *f, void *buf, uint64_t len, uint64_t off)
{
    if (!f || !f->used || !buf) return CM_ERR_INVAL;
    if (!can_read(f)) return CM_ERR_DENIED;
    cm_vfs_lock();
    int64_t r = g_mnt[f->mnt].ops->read(g_mnt[f->mnt].inst, f->ino, off, buf, len);
    cm_vfs_unlock();
    return r;
}

int64_t cm_vfs_pwrite(cm_file_t *f, const void *buf, uint64_t len, uint64_t off)
{
    if (!f || !f->used || !buf) return CM_ERR_INVAL;
    if (!can_write(f)) return CM_ERR_DENIED;
    cm_vfs_lock();
    int64_t r = g_mnt[f->mnt].ops->write(g_mnt[f->mnt].inst, f->ino, off, buf, len);
    cm_vfs_unlock();
    return r;
}

int64_t cm_vfs_read(cm_file_t *f, void *buf, uint64_t len)
{
    int64_t r = cm_vfs_pread(f, buf, len, f ? f->off : 0);
    if (r > 0) f->off += (uint64_t)r;
    return r;
}

int64_t cm_vfs_write(cm_file_t *f, const void *buf, uint64_t len)
{
    if (f && (f->mode & CM_O_APPEND)) {
        cm_stat_t st;
        cm_vfs_lock();
        if (g_mnt[f->mnt].ops->getattr(g_mnt[f->mnt].inst, f->ino, &st) == CM_OK)
            f->off = st.size;
        cm_vfs_unlock();
    }
    int64_t r = cm_vfs_pwrite(f, buf, len, f ? f->off : 0);
    if (r > 0) f->off += (uint64_t)r;
    return r;
}

cm_error_t cm_vfs_fsync(cm_file_t *f)
{
    if (!f || !f->used) return CM_ERR_INVAL;
    cm_vfs_lock();
    cm_error_t e = g_mnt[f->mnt].ops->sync(g_mnt[f->mnt].inst);
    cm_vfs_unlock();
    return e;
}

/* Lock-free on purpose: L4 jetsam closes a victim's files while holding
 * the mm lock, and vfs -> mm is the established lock order (the FS fills
 * the page cache). Taking the vfs lock here would invert it. */
cm_error_t cm_vfs_close(cm_file_t *f)
{
    if (!f || !f->used) return CM_ERR_INVAL;
    __atomic_store_n(&f->used, 0, __ATOMIC_RELEASE);
    return CM_OK;
}

uint32_t cm_vfs_open_files(void)
{
    uint32_t n = 0;
    for (int i = 0; i < VFS_MAX_FILES; i++) if (g_files[i].used) n++;
    return n;
}

/* ---- namespace -------------------------------------------------------- */

cm_error_t cm_vfs_stat(const char *path, cm_stat_t *out)
{
    if (!out) return CM_ERR_INVAL;
    cm_vfs_lock();
    int m = -1;
    cm_id_t ino = 0;
    cm_error_t e = resolve(path, 0, &m, &ino, NULL, NULL);
    if (e == CM_OK) e = g_mnt[m].ops->getattr(g_mnt[m].inst, ino, out);
    cm_vfs_unlock();
    return e;
}

cm_error_t cm_vfs_mkdir(const char *path)
{
    cm_vfs_lock();
    int m = -1;
    cm_id_t dir = 0, ino = 0;
    const char *leaf = NULL;
    size_t ll = 0;
    cm_error_t e = resolve(path, 1, &m, &dir, &leaf, &ll);
    if (e == CM_OK)
        e = g_mnt[m].ops->create(g_mnt[m].inst, dir, leaf, ll, CM_INO_DIR, 0, &ino);
    cm_vfs_unlock();
    return e;
}

cm_error_t cm_vfs_unlink(const char *path)
{
    cm_vfs_lock();
    int m = -1;
    cm_id_t dir = 0, ino = 0;
    const char *leaf = NULL;
    size_t ll = 0;
    cm_error_t e = resolve(path, 1, &m, &dir, &leaf, &ll);
    if (e == CM_OK) e = g_mnt[m].ops->lookup(g_mnt[m].inst, dir, leaf, ll, &ino);
    if (e == CM_OK)
        for (int i = 0; i < VFS_MAX_FILES; i++)
            if (g_files[i].used && g_files[i].mnt == m && g_files[i].ino == ino) {
                e = CM_ERR_BUSY;     /* no orphan inodes: keep it simple */
                break;
            }
    if (e == CM_OK) e = g_mnt[m].ops->unlink(g_mnt[m].inst, dir, leaf, ll);
    cm_vfs_unlock();
    return e;
}

cm_error_t cm_vfs_readdir(const char *path, uint32_t idx, cm_dirent_t *out)
{
    if (!out) return CM_ERR_INVAL;
    cm_vfs_lock();
    int m = -1;
    cm_id_t ino = 0;
    cm_error_t e = resolve(path, 0, &m, &ino, NULL, NULL);
    if (e == CM_OK) e = g_mnt[m].ops->readdir(g_mnt[m].inst, ino, idx, out);
    cm_vfs_unlock();
    return e;
}

cm_error_t cm_vfs_sync(void)
{
    cm_vfs_lock();
    cm_error_t e = CM_OK;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mnt[i].used) continue;
        cm_error_t r = g_mnt[i].ops->sync(g_mnt[i].inst);
        if (r != CM_OK) e = r;
    }
    cm_vfs_unlock();
    return e;
}

/* ---- COW volume helpers that need path resolution ---------------------- */

cm_error_t cm_cow_clone(const char *src, const char *dst)
{
    cm_vfs_lock();
    int ms = -1, md = -1;
    cm_id_t sino = 0, ddir = 0;
    const char *leaf = NULL;
    size_t ll = 0;
    cm_error_t e = resolve(src, 0, &ms, &sino, NULL, NULL);
    if (e == CM_OK) e = resolve(dst, 1, &md, &ddir, &leaf, &ll);
    if (e == CM_OK && (g_mnt[ms].ops != cm_cowfs_ops() || g_mnt[md].inst != NULL))
        e = CM_ERR_ROFS;   /* clone source may be a snapshot only if same fs */
    if (e == CM_OK && g_mnt[ms].inst != NULL) e = CM_ERR_NOTSUP;
    if (e == CM_OK) e = cm_cowfs_clone_ino(sino, ddir, leaf, ll, NULL);
    cm_vfs_unlock();
    if (e == CM_OK) kinfo("cowfs: clone %s -> %s (blocks shared, zero copy)", src, dst);
    return e;
}

uint32_t cm_cow_bmap(const char *path, uint64_t off)
{
    cm_vfs_lock();
    int m = -1;
    cm_id_t ino = 0;
    uint32_t b = 0;
    if (resolve(path, 0, &m, &ino, NULL, NULL) == CM_OK && g_mnt[m].inst == NULL)
        b = cm_cowfs_bmap_ino(ino, off);
    cm_vfs_unlock();
    return b;
}

cm_error_t cm_vfs_init(void)
{
    memset(g_mnt, 0, sizeof(g_mnt));
    memset(g_files, 0, sizeof(g_files));
    cm_error_t e = cm_cowfs_init();
    if (e != CM_OK) {
        kerr("vfs: COW volume unavailable (%d)", e);
        return e;
    }
    e = cm_vfs_mount("/", cm_cowfs_ops(), NULL);
    kinfo("vfs: '/' = cowfs (in-kernel, no IPC hop); snapshots under /.snap");
    return e;
}
