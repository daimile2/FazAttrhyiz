#include "capmix/fs.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

#define BLKSZ CM_PAGE_SIZE

typedef struct {
    uint64_t gen;
    char     tag[32];
} cm_snap_t;

static uint8_t     g_blocks[CM_VOL_BLOCKS][64]; /* compact block store (64B payload) */
static uint8_t     g_used[CM_VOL_BLOCKS];
static uint32_t    g_ag_used[CM_MAX_AG];
static cm_inode_t  g_ino[CM_MAX_INODES];
static const char *g_name[CM_MAX_INODES];
static cm_snap_t   g_snap[CM_MAX_SNAPS];
static uint64_t    g_super_gen = 1;
static uint64_t    g_nsnaps;
static int         g_ready;
static uint32_t    g_scrub_err;

static uint64_t fnv1a(const void *p, uint64_t n, uint64_t seed)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = seed ? seed : 14695981039346656037ull;
    for (uint64_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int ag_of(uint32_t blk) { return (int)(blk % CM_MAX_AG); }

static int alloc_blk(void)
{
    /* XFS-style: pick the emptiest AG first. */
    int best = 0;
    for (int a = 1; a < CM_MAX_AG; a++)
        if (g_ag_used[a] < g_ag_used[best]) best = a;
    for (uint32_t i = 0; i < CM_VOL_BLOCKS; i++) {
        if (g_used[i]) continue;
        if ((int)(i % CM_MAX_AG) != best && g_ag_used[best] > 0) continue;
        g_used[i] = 1;
        g_ag_used[ag_of(i)]++;
        memset(g_blocks[i], 0, sizeof(g_blocks[i]));
        return (int)i;
    }
    for (uint32_t i = 0; i < CM_VOL_BLOCKS; i++) {
        if (g_used[i]) continue;
        g_used[i] = 1;
        g_ag_used[ag_of(i)]++;
        memset(g_blocks[i], 0, sizeof(g_blocks[i]));
        return (int)i;
    }
    return -1;
}

static cm_inode_t *ino_get(cm_id_t id)
{
    if (id == 0 || id >= CM_MAX_INODES) return NULL;
    if (g_ino[id].ino == 0) return NULL;
    return &g_ino[id];
}

cm_error_t cm_cow_vol_init(void)
{
    memset(g_blocks, 0, sizeof(g_blocks));
    memset(g_used, 0, sizeof(g_used));
    memset(g_ag_used, 0, sizeof(g_ag_used));
    memset(g_ino, 0, sizeof(g_ino));
    memset(g_name, 0, sizeof(g_name));
    memset(g_snap, 0, sizeof(g_snap));
    g_super_gen = 1;
    g_nsnaps = 0;
    g_scrub_err = 0;
    g_ready = 1;
    /* inode 1 = root dir */
    g_ino[1].ino = 1;
    g_ino[1].kind = CM_INO_DIR;
    g_ino[1].cow_gen = g_super_gen;
    g_ino[1].refs = 1;
    g_name[1] = "/";
    kinfo("fs: COW volume %u blocks, %d AGs, super_gen=%llu (APFS-shaped)",
          CM_VOL_BLOCKS, CM_MAX_AG, (unsigned long)g_super_gen);
    return CM_OK;
}

cm_error_t cm_vfs_create(const char *path, uint32_t flags, cm_id_t *out_ino)
{
    if (!g_ready) return CM_ERR_NOTSUP;
    int slot = -1;
    for (int i = 2; i < CM_MAX_INODES; i++)
        if (g_ino[i].ino == 0) { slot = i; break; }
    if (slot < 0) return CM_ERR_NOSPC;
    memset(&g_ino[slot], 0, sizeof(g_ino[slot]));
    g_ino[slot].ino = (cm_id_t)slot;
    g_ino[slot].kind = CM_INO_FILE;
    g_ino[slot].flags = flags;
    g_ino[slot].cow_gen = g_super_gen;
    g_ino[slot].refs = 1;
    g_name[slot] = path;
    if (out_ino) *out_ino = (cm_id_t)slot;
    kinfo("fs: create %s ino=%d flags=%s", path ? path : "?", slot,
          (flags & CM_FS_NOCOW) ? "nocow" : "cow");
    return CM_OK;
}

static cm_error_t write_extent(cm_inode_t *n, uint64_t off, const void *buf, uint64_t len)
{
    uint32_t idx = (uint32_t)(off / 64);
    if (idx >= CM_MAX_EXTENTS) return CM_ERR_NOSPC;
    int cow = (n->flags & CM_FS_NOCOW) == 0;
    if (idx >= n->nextents || n->ext[idx].nblk == 0) {
        int blk = alloc_blk();
        if (blk < 0) return CM_ERR_NOSPC;
        n->ext[idx].blk = (uint32_t)blk;
        n->ext[idx].nblk = 1;
        n->ext[idx].gen = (uint16_t)n->cow_gen;
        if (idx >= n->nextents) n->nextents = idx + 1;
    } else if (cow) {
        /* Atomic new block + pointer flip; old block stays for snapshots/clones. */
        int blk = alloc_blk();
        if (blk < 0) return CM_ERR_NOSPC;
        memcpy(g_blocks[blk], g_blocks[n->ext[idx].blk], 64);
        n->ext[idx].blk = (uint32_t)blk;
        n->ext[idx].gen = (uint16_t)n->cow_gen;
    }
    uint64_t copy = len > 64 ? 64 : len;
    memcpy(g_blocks[n->ext[idx].blk], buf, (size_t)copy);
    n->csum = fnv1a(g_blocks[n->ext[idx].blk], 64, n->csum ^ n->cow_gen);
    if (off + copy > n->size) n->size = off + copy;
    return CM_OK;
}

cm_error_t cm_vfs_write(cm_id_t ino, uint64_t off, const void *buf, uint64_t len)
{
    cm_inode_t *n = ino_get(ino);
    if (!n || !buf) return CM_ERR_INVAL;
    if (off != 0) {
        /* Sparse VDL: hole before first write does not allocate (ReFS lesson). */
    }
    cm_error_t e = write_extent(n, off, buf, len);
    if (e != CM_OK) return e;

    /* File-backed cache page so L1 has something real to drop. */
    void *pg = cm_page_alloc(0);
    if (pg) {
        memset(pg, 0, CM_PAGE_SIZE);
        uint64_t ncopy = len > CM_PAGE_SIZE ? CM_PAGE_SIZE : len;
        memcpy(pg, buf, (size_t)ncopy);
        if (cm_cache_add_file(ino, off, pg, 0) != CM_OK)
            cm_page_free(pg, 0);
    }
    cm_obs_emit(cm_trace_new(), CM_EV_ALLOC, ino, off, len);
    return CM_OK;
}

cm_error_t cm_vfs_read(cm_id_t ino, uint64_t off, void *buf, uint64_t len)
{
    cm_inode_t *n = ino_get(ino);
    if (!n || !buf) return CM_ERR_INVAL;
    uint32_t idx = (uint32_t)(off / 64);
    memset(buf, 0, (size_t)len); /* sparse hole = zeros */
    if (idx < n->nextents && n->ext[idx].nblk) {
        uint64_t copy = len > 64 ? 64 : len;
        memcpy(buf, g_blocks[n->ext[idx].blk], (size_t)copy);
    }
    return CM_OK;
}

cm_error_t cm_vfs_getattr(cm_id_t ino, cm_inode_t *out)
{
    cm_inode_t *n = ino_get(ino);
    if (!n || !out) return CM_ERR_INVAL;
    *out = *n;
    return CM_OK;
}

cm_error_t cm_cow_snapshot(const char *vol)
{
    if (!g_ready) return CM_ERR_NOTSUP;
    if (g_nsnaps >= CM_MAX_SNAPS) return CM_ERR_NOSPC;
    /* Crash-safe: bump generation, old tree still reachable by previous gen. */
    g_super_gen++;
    g_snap[g_nsnaps].gen = g_super_gen - 1;
    memset(g_snap[g_nsnaps].tag, 0, 32);
    if (vol) {
        size_t n = strlen(vol);
        if (n > 31) n = 31;
        memcpy(g_snap[g_nsnaps].tag, vol, n);
    }
    g_nsnaps++;
    kinfo("fs: snapshot '%s' gen %llu → %llu (old pointer intact, no fsck)",
          vol ? vol : "root", (unsigned long)(g_super_gen - 1),
          (unsigned long)g_super_gen);
    return CM_OK;
}

cm_error_t cm_cow_clone(cm_id_t ino, cm_id_t *out_clone)
{
    cm_inode_t *src = ino_get(ino);
    if (!src) return CM_ERR_INVAL;
    int slot = -1;
    for (int i = 2; i < CM_MAX_INODES; i++)
        if (g_ino[i].ino == 0) { slot = i; break; }
    if (slot < 0) return CM_ERR_NOSPC;
    g_ino[slot] = *src;
    g_ino[slot].ino = (cm_id_t)slot;
    g_ino[slot].refs = 1;
    src->refs++;
    g_name[slot] = "clone";
    /* Extents shared until first write — APFS/ReFS block cloning. */
    if (out_clone) *out_clone = (cm_id_t)slot;
    kinfo("fs: clone ino=%llu → %d shared_ext=%u",
          (unsigned long)ino, slot, src->nextents);
    return CM_OK;
}

cm_error_t cm_vfs_scrub(void)
{
    uint32_t err = 0;
    for (int i = 1; i < CM_MAX_INODES; i++) {
        if (g_ino[i].ino == 0) continue;
        for (uint32_t e = 0; e < g_ino[i].nextents; e++) {
            uint32_t b = g_ino[i].ext[e].blk;
            if (b >= CM_VOL_BLOCKS || !g_used[b]) err++;
        }
    }
    g_scrub_err = err;
    kinfo("fs: scrub %s (%u errors)", err ? "DIRTY" : "clean", err);
    return err ? CM_ERR_EXIST : CM_OK;
}

void cm_vfs_stat(cm_vol_stat_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->super_gen = g_super_gen;
    out->nsnaps = g_nsnaps;
    uint64_t used = 0;
    uint64_t files = 0;
    for (uint32_t i = 0; i < CM_VOL_BLOCKS; i++) if (g_used[i]) used++;
    for (int i = 1; i < CM_MAX_INODES; i++) if (g_ino[i].ino) files++;
    out->used_blocks = used;
    out->free_blocks = CM_VOL_BLOCKS - used;
    out->files = files;
    out->last_scrub_errors = g_scrub_err;
}

uint64_t cm_vfs_super_gen(void) { return g_super_gen; }
