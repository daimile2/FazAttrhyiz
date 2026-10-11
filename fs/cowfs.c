#include "capmix/trace.h"
#include "capmix/fs.h"
#include "capmix/io.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/obs.h"
#include "capmix/arch.h"

#include <string.h>

/* COW volume on the blk device's fs partition. Layout and invariants are
 * described in fs.h; the short version:
 *
 *  - Nothing reachable from the last committed superblock is ever written
 *    (except nocow file data). New versions go to free blocks; the commit
 *    ends with ONE sector write of the superblock into the other slot.
 *    Crash anywhere before that sector and mount sees the old tree, whole.
 *  - "Free" is computed, not stored: a block is free iff no committed root
 *    (live tree or any snapshot) reaches it and the current transaction has
 *    not claimed it. Recomputed after every commit by walking the roots, so
 *    there is no free-space map to get out of sync and nothing to fsck.
 *  - Every pointer carries the checksum of its target. Metadata is DUP. */

#define FS_BS        4096u
#define FS_SPB       (FS_BS / CM_BLK_SECTOR)
#define FS_NBLK      (CM_BLK_FS_SECT / FS_SPB)
#define FS_MAGIC     0x3153465843504143ull         /* "CAPCXFS1" */
#define FS_NINODES   256u
#define FS_IPB       16u
#define FS_NITAB     (FS_NINODES / FS_IPB)
#define FS_NDIRECT   12u
#define FS_NIND      (FS_BS / sizeof(fs_bptr_t))
#define FS_MAXFBLK   (FS_NDIRECT + FS_NIND)
#define FS_ROOT_INO  1u
#define FS_DIRENT_SZ 64u
#define FS_DPB       (FS_BS / FS_DIRENT_SZ)
#define FS_FIRST_BLK 2u                             /* 0,1 = superblock slots */

typedef struct {
    uint32_t blk;      /* 0 = hole / none */
    uint32_t dup;      /* second copy (metadata only), 0 for data */
    uint64_t csum;     /* checksum of the 4 KiB target; 0 for nocow data */
} fs_bptr_t;

typedef struct {
    uint32_t  ino;
    uint16_t  kind;
    uint16_t  flags;
    uint64_t  size;
    uint32_t  nlink;
    uint32_t  parent;
    uint64_t  gen;
    fs_bptr_t d[FS_NDIRECT];
    fs_bptr_t ind;
    uint8_t   pad[16];
} fs_dinode_t;

typedef struct {
    uint64_t  gen;
    uint64_t  ctime;
    char      name[16];
    fs_bptr_t omap;
} fs_snap_t;

typedef struct {
    uint64_t  magic;
    uint64_t  gen;
    uint32_t  nblocks;
    uint32_t  ninodes;
    uint32_t  nsnaps;
    uint32_t  pad;
    fs_bptr_t omap;
    fs_snap_t snaps[CM_MAX_SNAPS];
    uint64_t  csum;    /* over every byte above */
} fs_super_t;

typedef struct {
    uint32_t ino;
    uint8_t  kind;
    uint8_t  nlen;
    char     name[FS_DIRENT_SZ - 6];
} fs_dirent_t;

/* Compile-time layout checks: these sizes ARE the on-disk format. */
typedef char fs_chk_inode[(sizeof(fs_dinode_t) == 256) ? 1 : -1];
typedef char fs_chk_super[(sizeof(fs_super_t) <= CM_BLK_SECTOR) ? 1 : -1];
typedef char fs_chk_dirent[(sizeof(fs_dirent_t) == FS_DIRENT_SZ) ? 1 : -1];
typedef char fs_chk_omap[(FS_NITAB * sizeof(fs_bptr_t) <= FS_BS) ? 1 : -1];

/* ---- mounted state ---------------------------------------------------- */

static fs_super_t   g_sb;                      /* last COMMITTED superblock */
static fs_dinode_t  g_ino[FS_NINODES];         /* live inode table (may be dirty) */
static fs_bptr_t    g_omap[FS_NITAB];          /* live itab pointers */
static uint8_t      g_itab_dirty[FS_NITAB];
static fs_bptr_t   *g_ind[FS_NINODES];         /* loaded indirect blocks (page each) */
static uint8_t      g_ind_dirty[FS_NINODES];
static uint8_t      g_ref[FS_NBLK];            /* refs from committed roots, saturating */
static uint8_t      g_pend[FS_NBLK];           /* claimed by the open transaction */
static uint8_t      g_shared[FS_NBLK];         /* shared by an uncommitted clone */
static int          g_sb_dirty;
static int          g_mounted;
static int          g_crashed;
static int          g_readonly;
static int          g_txn_depth;
static int          g_crash_point;
static uint32_t     g_rotor[CM_MAX_AG];

static uint64_t     g_commits, g_cow_writes, g_inplace, g_csum_err, g_read_repair;

/* Scratch buffers. Walks nest at most omap -> itab -> ind, so three levels
 * plus one for data. The FS is single-threaded (vfs.c serialises entry). */
static uint8_t g_buf_omap[FS_BS] __attribute__((aligned(16)));
static uint8_t g_buf_itab[FS_BS] __attribute__((aligned(16)));
static uint8_t g_buf_ind[FS_BS]  __attribute__((aligned(16)));
static uint8_t g_buf_data[FS_BS] __attribute__((aligned(16)));
static uint8_t g_buf_aux[FS_BS]  __attribute__((aligned(16)));

/* ---- primitives ------------------------------------------------------- */

static uint64_t fs_csum(const void *p, uint64_t n)
{
    /* FNV-1a over 64-bit lanes with a final avalanche. Not cryptographic —
     * it is here to catch bit rot and torn/misdirected writes, the same job
     * Btrfs gives crc32c. A zero result is remapped so 0 can mean "none". */
    const uint64_t *w = (const uint64_t *)p;
    uint64_t h = 14695981039346656037ull;
    for (uint64_t i = 0; i < n / 8; i++) {
        h ^= w[i];
        h *= 1099511628211ull;
        h ^= h >> 29;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h ? h : 1;
}

static uint64_t sb_csum(const fs_super_t *s)
{
    return fs_csum(s, offsetof(fs_super_t, csum));
}

uint64_t cm_cow_fs_lba(uint32_t blk)
{
    return CM_BLK_FS_LBA + (uint64_t)blk * FS_SPB;
}

static cm_error_t bread(uint32_t blk, void *buf)
{
    if (blk >= FS_NBLK) return CM_ERR_INVAL;
    return cm_blk_rw(CM_OP_READ, cm_cow_fs_lba(blk), FS_SPB, buf);
}

static cm_error_t bwrite(uint32_t blk, const void *buf)
{
    if (blk >= FS_NBLK) return CM_ERR_INVAL;
    return cm_blk_rw(CM_OP_WRITE, cm_cow_fs_lba(blk), FS_SPB, (void *)buf);
}

static int ag_of(uint32_t blk) { return (int)((blk * CM_MAX_AG) / FS_NBLK); }

static int blk_busy(uint32_t b)
{
    return b < FS_FIRST_BLK || g_ref[b] || g_pend[b];
}

/* XFS-style allocation groups: each inode prefers one AG (ino % 4), so
 * unrelated files do not interleave and four writers would not contend on
 * one rotor. Falls back to any AG when the preferred one is full. */
static uint32_t alloc_blk(uint32_t hint_ino)
{
    int ag0 = (int)(hint_ino % CM_MAX_AG);
    for (int k = 0; k < CM_MAX_AG; k++) {
        int ag = (ag0 + k) % CM_MAX_AG;
        uint32_t lo = (uint32_t)((uint64_t)ag * FS_NBLK / CM_MAX_AG);
        uint32_t hi = (uint32_t)((uint64_t)(ag + 1) * FS_NBLK / CM_MAX_AG);
        if (lo < FS_FIRST_BLK) lo = FS_FIRST_BLK;
        uint32_t span = hi - lo;
        uint32_t start = g_rotor[ag] >= lo && g_rotor[ag] < hi ? g_rotor[ag] : lo;
        for (uint32_t i = 0; i < span; i++) {
            uint32_t b = lo + (start - lo + i) % span;
            if (blk_busy(b)) continue;
            g_pend[b] = 1;
            g_rotor[ag] = b + 1;
            cm_pcache_invalidate(b);    /* old content of a recycled block */
            return b;
        }
    }
    return 0;
}

/* Read the block behind p and verify it. Metadata gets a second chance
 * from its DUP copy, and the bad copy is rewritten from the good one
 * (Btrfs read-repair). verify=0 is for nocow data, which has no checksum. */
static cm_error_t read_ptr(const fs_bptr_t *p, void *buf, int verify)
{
    if (!p->blk) { memset(buf, 0, FS_BS); return CM_OK; }
    cm_error_t e = bread(p->blk, buf);
    if (e != CM_OK) return e;
    if (!verify || fs_csum(buf, FS_BS) == p->csum) return CM_OK;
    g_csum_err++;
    if (p->dup) {
        e = bread(p->dup, buf);
        if (e == CM_OK && fs_csum(buf, FS_BS) == p->csum) {
            if (!g_readonly && bwrite(p->blk, buf) == CM_OK) g_read_repair++;
            kwarn("cowfs: blk %u bad checksum, healed from DUP copy %u",
                  p->blk, p->dup);
            return CM_OK;
        }
    }
    kerr("cowfs: blk %u checksum mismatch (want %llx) — EIO",
         p->blk, (unsigned long)p->csum);
    return CM_ERR_IO;
}

/* Write a metadata block twice to two fresh locations. */
static cm_error_t write_meta(const void *buf, uint32_t hint, fs_bptr_t *out)
{
    uint32_t a = alloc_blk(hint), b = alloc_blk(hint + 1);
    if (!a || !b) return CM_ERR_NOSPC;
    cm_error_t e = bwrite(a, buf);
    if (e == CM_OK) e = bwrite(b, buf);
    if (e != CM_OK) return e;
    out->blk = a;
    out->dup = b;
    out->csum = fs_csum(buf, FS_BS);
    return CM_OK;
}

/* ---- reachability walk (free space + scrub) --------------------------- */

static void ref_blk(uint32_t b)
{
    if (b && b < FS_NBLK && g_ref[b] < 255) g_ref[b]++;
}

typedef struct {
    int                 scrub;
    cm_scrub_report_t  *rep;
    uint8_t            *seen;       /* data blocks already verified */
    int                 failed;
} walk_t;

/* Scrub a metadata pointer: check BOTH copies independently and repair
 * the bad one. Normal reads stop at the first good copy, so a rotten DUP
 * copy would otherwise sit undetected until the good one rots too. */
static cm_error_t scrub_meta(const fs_bptr_t *p, void *buf, walk_t *w)
{
    w->rep->meta_checked++;
    int ok_a = bread(p->blk, buf) == CM_OK && fs_csum(buf, FS_BS) == p->csum;
    int ok_b = p->dup && bread(p->dup, g_buf_aux) == CM_OK &&
               fs_csum(g_buf_aux, FS_BS) == p->csum;
    if (ok_a && (ok_b || !p->dup)) return CM_OK;
    if (!ok_a && !ok_b) { w->rep->meta_lost++; return CM_ERR_IO; }
    if (ok_a) {
        if (bwrite(p->dup, buf) == CM_OK) w->rep->meta_repaired++;
    } else {
        memcpy(buf, g_buf_aux, FS_BS);
        if (bwrite(p->blk, buf) == CM_OK) w->rep->meta_repaired++;
    }
    kwarn("cowfs: scrub repaired metadata blk %u/%u", p->blk, p->dup);
    return CM_OK;
}

static cm_error_t walk_meta(const fs_bptr_t *p, void *buf, walk_t *w)
{
    ref_blk(p->blk);
    ref_blk(p->dup);
    return w->scrub ? scrub_meta(p, buf, w) : read_ptr(p, buf, 1);
}

static void walk_data(const fs_bptr_t *p, int nocow, walk_t *w)
{
    if (!p->blk) return;
    ref_blk(p->blk);
    if (!w->scrub || nocow || w->seen[p->blk]) return;
    w->seen[p->blk] = 1;
    w->rep->data_checked++;
    if (bread(p->blk, g_buf_data) != CM_OK ||
        fs_csum(g_buf_data, FS_BS) != p->csum) {
        w->rep->data_errors++;
        kerr("cowfs: scrub: data blk %u checksum mismatch (no redundant copy)",
             p->blk);
    }
}

static void walk_root(const fs_bptr_t *omap, walk_t *w)
{
    if (walk_meta(omap, g_buf_omap, w) != CM_OK) { w->failed = 1; return; }
    fs_bptr_t itp[FS_NITAB];
    memcpy(itp, g_buf_omap, sizeof(itp));
    for (uint32_t i = 0; i < FS_NITAB; i++) {
        if (!itp[i].blk) continue;
        if (walk_meta(&itp[i], g_buf_itab, w) != CM_OK) { w->failed = 1; continue; }
        const fs_dinode_t *di = (const fs_dinode_t *)g_buf_itab;
        for (uint32_t k = 0; k < FS_IPB; k++) {
            if (!di[k].ino) continue;
            int nocow = (di[k].flags & CM_FS_NOCOW) != 0;
            for (uint32_t d = 0; d < FS_NDIRECT; d++) walk_data(&di[k].d[d], nocow, w);
            if (di[k].ind.blk) {
                if (walk_meta(&di[k].ind, g_buf_ind, w) != CM_OK) { w->failed = 1; continue; }
                const fs_bptr_t *ip = (const fs_bptr_t *)g_buf_ind;
                for (uint32_t j = 0; j < FS_NIND; j++) walk_data(&ip[j], nocow, w);
            }
        }
    }
}

static int walk_all(walk_t *w)
{
    memset(g_ref, 0, sizeof(g_ref));
    g_ref[0] = g_ref[1] = 1;
    w->failed = 0;
    walk_root(&g_sb.omap, w);
    for (uint32_t s = 0; s < g_sb.nsnaps; s++) walk_root(&g_sb.snaps[s].omap, w);
    if (w->failed && !g_readonly) {
        /* A subtree we could not read is a subtree whose blocks we could not
         * count — they would look free and get recycled. Stop writing. */
        g_readonly = 1;
        kerr("cowfs: unreadable metadata during walk — volume now read-only");
    }
    return w->failed ? -1 : 0;
}

static void recount(void)
{
    cm_scrub_report_t dummy;
    walk_t w = { 0, &dummy, NULL, 0 };
    walk_all(&w);
    memset(g_pend, 0, sizeof(g_pend));
    memset(g_shared, 0, sizeof(g_shared));
}

/* ---- commit ----------------------------------------------------------- */

void cm_cow_inject_crash(int point) { g_crash_point = point; }

static cm_error_t crash_here(int point)
{
    if (g_crash_point != point) return CM_OK;
    g_crash_point = CM_CRASH_NONE;
    g_crashed = 1;
    kwarn("cowfs: *** simulated power cut at commit stage %d ***", point);
    return CM_ERR_DEAD;
}

static int any_dirty(void)
{
    if (g_sb_dirty) return 1;
    for (uint32_t i = 0; i < FS_NITAB; i++) if (g_itab_dirty[i]) return 1;
    return 0;
}

static cm_error_t commit(void)
{
    if (g_crashed) return CM_ERR_DEAD;
    if (g_readonly) return CM_ERR_ROFS;
    if (!any_dirty()) return CM_OK;
    cm_error_t e;

    if ((e = crash_here(CM_CRASH_AFTER_DATA)) != CM_OK) return e;

    /* 1. indirect blocks of modified inodes */
    for (uint32_t i = 0; i < FS_NINODES; i++) {
        if (!g_ind_dirty[i] || !g_ind[i]) continue;
        if ((e = write_meta(g_ind[i], i, &g_ino[i].ind)) != CM_OK) return e;
        g_ind_dirty[i] = 0;
        g_itab_dirty[i / FS_IPB] = 1;
    }
    /* 2. inode-table blocks */
    for (uint32_t t = 0; t < FS_NITAB; t++) {
        if (!g_itab_dirty[t]) continue;
        memcpy(g_buf_itab, &g_ino[t * FS_IPB], FS_BS);
        if ((e = write_meta(g_buf_itab, t, &g_omap[t])) != CM_OK) return e;
    }
    if ((e = crash_here(CM_CRASH_AFTER_ITAB)) != CM_OK) return e;

    /* 3. object map */
    memset(g_buf_omap, 0, FS_BS);
    memcpy(g_buf_omap, g_omap, sizeof(g_omap));
    fs_super_t ns = g_sb;
    if ((e = write_meta(g_buf_omap, 0, &ns.omap)) != CM_OK) return e;
    if ((e = crash_here(CM_CRASH_AFTER_OMAP)) != CM_OK) return e;

    /* 4. the flip: one sector, into the slot the current super is NOT in */
    ns.gen = g_sb.gen + 1;
    ns.csum = sb_csum(&ns);
    uint8_t sec[CM_BLK_SECTOR];
    memset(sec, 0, sizeof(sec));
    memcpy(sec, &ns, sizeof(ns));
    uint64_t lba = cm_cow_fs_lba((uint32_t)(ns.gen & 1));
    if (g_crash_point == CM_CRASH_TORN_SUPER) {
        /* Half the sector reached the platter: new header, old tail. */
        uint8_t old[CM_BLK_SECTOR];
        if (cm_blk_rw(CM_OP_READ, lba, 1, old) == CM_OK) {
            memcpy(old, sec, CM_BLK_SECTOR / 2);
            cm_blk_rw(CM_OP_WRITE, lba, 1, old);
        }
        return crash_here(CM_CRASH_TORN_SUPER);
    }
    if ((e = cm_blk_rw(CM_OP_WRITE, lba, 1, sec)) != CM_OK) return e;

    g_sb = ns;
    g_sb_dirty = 0;
    memset(g_itab_dirty, 0, sizeof(g_itab_dirty));
    g_commits++;
    recount();
    cm_obs_emit(cm_trace_new(), CM_EV_FS, 0x10, g_sb.gen, 0);
    return CM_OK;
}

void cm_cow_txn_begin(void) { cm_vfs_lock(); g_txn_depth++; cm_vfs_unlock(); }

static cm_error_t cm_cow_txn_end_locked(void)
{
    if (g_txn_depth > 0) g_txn_depth--;
    return g_txn_depth == 0 ? commit() : CM_OK;
}

/* ---- inode helpers (live tree) ---------------------------------------- */

static int live_ok(void)
{
    if (!g_mounted || g_crashed) return CM_ERR_DEAD;
    return CM_OK;
}

static fs_dinode_t *ino_get(cm_id_t ino)
{
    if (ino == 0 || ino >= FS_NINODES || g_ino[ino].ino != ino) return NULL;
    return &g_ino[ino];
}

static void ino_dirty(uint32_t ino)
{
    g_itab_dirty[ino / FS_IPB] = 1;
    g_ino[ino].gen = g_sb.gen + 1;
}

static cm_error_t ind_load(fs_dinode_t *di, int create)
{
    uint32_t i = di->ino;
    if (g_ind[i]) return CM_OK;
    if (!di->ind.blk && !create) return CM_ERR_NOENT;
    fs_bptr_t *pg = (fs_bptr_t *)cm_page_alloc(0);
    if (!pg) return CM_ERR_NOMEM;
    if (di->ind.blk) {
        cm_error_t e = read_ptr(&di->ind, pg, 1);
        if (e != CM_OK) { cm_page_free(pg, 0); return e; }
    } else {
        memset(pg, 0, FS_BS);
    }
    g_ind[i] = pg;
    return CM_OK;
}

static void ind_drop(uint32_t i)
{
    if (g_ind[i]) cm_page_free(g_ind[i], 0);
    g_ind[i] = NULL;
    g_ind_dirty[i] = 0;
}

/* Pointer slot for file block fb of a LIVE inode; NULL past the max size
 * or (create=0) inside a hole of an unloaded indirect. */
static fs_bptr_t *live_bptr(fs_dinode_t *di, uint64_t fb, int create)
{
    if (fb < FS_NDIRECT) return &di->d[fb];
    if (fb >= FS_MAXFBLK) return NULL;
    if (ind_load(di, create) != CM_OK) return NULL;
    if (create) { g_ind_dirty[di->ino] = 1; ino_dirty(di->ino); }
    return &g_ind[di->ino][fb - FS_NDIRECT];
}

/* ---- views: one read path for the live tree and for snapshots ----------
 * snap < 0 is the live tree; otherwise an index into g_sb.snaps[]. Snapshot
 * inodes are read straight from disk through the snapshot's own omap, so a
 * snapshot is exactly what was committed at that generation. */

static cm_error_t snap_inode(int snap, uint32_t ino, fs_dinode_t *out)
{
    if (snap < 0 || (uint32_t)snap >= g_sb.nsnaps || ino >= FS_NINODES)
        return CM_ERR_INVAL;
    cm_error_t e = read_ptr(&g_sb.snaps[snap].omap, g_buf_omap, 1);
    if (e != CM_OK) return e;
    const fs_bptr_t *itp = (const fs_bptr_t *)g_buf_omap;
    fs_bptr_t it = itp[ino / FS_IPB];
    if (!it.blk) return CM_ERR_NOENT;
    if ((e = read_ptr(&it, g_buf_itab, 1)) != CM_OK) return e;
    *out = ((const fs_dinode_t *)g_buf_itab)[ino % FS_IPB];
    return out->ino == ino ? CM_OK : CM_ERR_NOENT;
}

static cm_error_t view_inode(int snap, uint32_t ino, fs_dinode_t *out)
{
    if (snap < 0) {
        fs_dinode_t *di = ino_get(ino);
        if (!di) return CM_ERR_NOENT;
        *out = *di;
        return CM_OK;
    }
    return snap_inode(snap, ino, out);
}

static cm_error_t read_data(const fs_bptr_t *p, int nocow, void *buf)
{
    if (!p->blk) { memset(buf, 0, FS_BS); return CM_OK; }
    if (cm_pcache_read(p->blk, buf)) return CM_OK;
    cm_error_t e = read_ptr(p, buf, !nocow);
    if (e == CM_OK) cm_pcache_fill(p->blk, buf);
    return e;
}

static int64_t view_read(int snap, uint32_t ino, uint64_t off, void *buf, uint64_t len)
{
    fs_dinode_t di;
    cm_error_t e = view_inode(snap, ino, &di);
    if (e != CM_OK) return e;
    if (off >= di.size) return 0;
    if (len > di.size - off) len = di.size - off;
    int nocow = (di.flags & CM_FS_NOCOW) != 0;
    const fs_bptr_t *ind = (snap < 0) ? g_ind[ino] : NULL;
    int ind_tmp = 0;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t fb = pos / FS_BS, bo = pos % FS_BS;
        uint64_t n = FS_BS - bo;
        if (n > len - done) n = len - done;
        fs_bptr_t p = { 0, 0, 0 };
        if (fb < FS_NDIRECT) {
            p = di.d[fb];
        } else if (fb < FS_MAXFBLK && (ind || di.ind.blk)) {
            if (!ind) {
                if ((e = read_ptr(&di.ind, g_buf_ind, 1)) != CM_OK) return e;
                ind = (const fs_bptr_t *)g_buf_ind;
                ind_tmp = 1;
            }
            p = ind[fb - FS_NDIRECT];
        }
        if ((e = read_data(&p, nocow, g_buf_data)) != CM_OK) return e;
        memcpy((uint8_t *)buf + done, g_buf_data + bo, (size_t)n);
        done += n;
    }
    (void)ind_tmp;
    return (int64_t)done;
}

/* ---- live writes ------------------------------------------------------ */

static int64_t live_write(uint32_t ino, uint64_t off, const void *buf, uint64_t len)
{
    fs_dinode_t *di = ino_get(ino);
    if (!di) return CM_ERR_NOENT;
    if (g_readonly) return CM_ERR_ROFS;
    if (off + len > (uint64_t)FS_MAXFBLK * FS_BS) return CM_ERR_NOSPC;
    int nocow = (di->flags & CM_FS_NOCOW) != 0;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t fb = pos / FS_BS, bo = pos % FS_BS;
        uint64_t n = FS_BS - bo;
        if (n > len - done) n = len - done;
        fs_bptr_t *p = live_bptr(di, fb, 1);
        if (!p) return done ? (int64_t)done : CM_ERR_NOMEM;

        /* Read-modify-write for a partial block over existing data. */
        if (p->blk && n < FS_BS) {
            cm_error_t e = read_data(p, nocow, g_buf_data);
            if (e != CM_OK) return e;
        } else if (n < FS_BS) {
            memset(g_buf_data, 0, FS_BS);
        }
        memcpy(g_buf_data + bo, (const uint8_t *)buf + done, (size_t)n);

        /* nocow may overwrite in place only if NOTHING else can see the
         * block: not a snapshot (ref would be >1), not an uncommitted clone
         * (g_shared). A shared block is COWed even for nocow — Btrfs does
         * the same, otherwise writing a nocow file would rewrite history. */
        uint32_t b = p->blk;
        int inplace = nocow && b && !g_shared[b] && (g_pend[b] || g_ref[b] == 1);
        if (!inplace) {
            b = alloc_blk(ino);
            if (!b) return done ? (int64_t)done : CM_ERR_NOSPC;
        }
        cm_error_t e = bwrite(b, g_buf_data);
        if (e != CM_OK) return e;
        cm_pcache_invalidate(b);
        cm_pcache_fill(b, g_buf_data);
        if (inplace) g_inplace++; else g_cow_writes++;
        if (!inplace && p->blk) CM_TRACE(COW_SPLIT,0,ino,p->blk,b);
        p->blk = b;
        p->dup = 0;
        p->csum = nocow ? 0 : fs_csum(g_buf_data, FS_BS);
        if (fb >= FS_NDIRECT) g_ind_dirty[ino] = 1;
        done += n;
    }
    if (off + len > di->size) di->size = off + len;
    ino_dirty(ino);
    return (int64_t)done;
}

static cm_error_t live_truncate(uint32_t ino, uint64_t size)
{
    fs_dinode_t *di = ino_get(ino);
    if (!di) return CM_ERR_NOENT;
    if (g_readonly) return CM_ERR_ROFS;
    if (size >= di->size) { di->size = size; ino_dirty(ino); return CM_OK; }
    uint64_t keep = (size + FS_BS - 1) / FS_BS;   /* blocks still needed */
    for (uint64_t fb = keep; fb < FS_NDIRECT; fb++) di->d[fb].blk = 0, di->d[fb].csum = 0;
    if (keep <= FS_NDIRECT) {
        ind_drop(ino);
        memset(&di->ind, 0, sizeof(di->ind));
    } else if (ind_load(di, 0) == CM_OK) {
        for (uint64_t fb = keep; fb < FS_MAXFBLK; fb++)
            memset(&g_ind[ino][fb - FS_NDIRECT], 0, sizeof(fs_bptr_t));
        g_ind_dirty[ino] = 1;
    }
    /* Zero the tail of the last kept block so a later extend reads zeros. */
    uint64_t tail = size % FS_BS;
    if (tail) {
        uint8_t z[256];
        memset(z, 0, sizeof(z));
        uint64_t pos = size, end = keep * FS_BS;
        if (end > di->size) end = di->size;
        while (pos < end) {
            uint64_t n = end - pos > sizeof(z) ? sizeof(z) : end - pos;
            int64_t w = live_write(ino, pos, z, n);
            if (w < 0) return (cm_error_t)w;
            pos += n;
        }
    }
    di->size = size;
    ino_dirty(ino);
    return CM_OK;
}

/* ---- directories ------------------------------------------------------ */

static int name_eq(const fs_dirent_t *d, const char *name, size_t nlen)
{
    return d->ino && d->nlen == nlen && memcmp(d->name, name, nlen) == 0;
}

static cm_error_t dir_find(int snap, uint32_t dir, const char *name, size_t nlen,
                           uint32_t *out_ino, uint64_t *out_off)
{
    fs_dinode_t di;
    cm_error_t e = view_inode(snap, dir, &di);
    if (e != CM_OK) return e;
    if (di.kind != CM_INO_DIR) return CM_ERR_NOTDIR;
    fs_dirent_t de;
    for (uint64_t off = 0; off < di.size; off += FS_DIRENT_SZ) {
        int64_t r = view_read(snap, dir, off, &de, sizeof(de));
        if (r < 0) return (cm_error_t)r;
        if (r < (int64_t)sizeof(de)) break;
        if (name_eq(&de, name, nlen)) {
            if (out_ino) *out_ino = de.ino;
            if (out_off) *out_off = off;
            return CM_OK;
        }
    }
    return CM_ERR_NOENT;
}

static cm_error_t dir_add(uint32_t dir, const char *name, size_t nlen,
                          uint32_t ino, uint8_t kind)
{
    fs_dinode_t *d = ino_get(dir);
    if (!d) return CM_ERR_NOENT;
    fs_dirent_t de;
    uint64_t slot = d->size;
    for (uint64_t off = 0; off < d->size; off += FS_DIRENT_SZ) {
        if (view_read(-1, dir, off, &de, sizeof(de)) < (int64_t)sizeof(de)) break;
        if (!de.ino) { slot = off; break; }
    }
    memset(&de, 0, sizeof(de));
    de.ino = ino;
    de.kind = kind;
    de.nlen = (uint8_t)nlen;
    memcpy(de.name, name, nlen);
    int64_t w = live_write(dir, slot, &de, sizeof(de));
    return w < 0 ? (cm_error_t)w : CM_OK;
}

/* ---- ops for the VFS -------------------------------------------------- */

/* inst encodes the view: NULL = live, (snap+1) cast to a pointer otherwise. */
static int inst_snap(void *inst) { return inst ? (int)(uintptr_t)inst - 1 : -1; }

static cm_id_t op_root(void *inst) { (void)inst; return FS_ROOT_INO; }

static int op_lookup(void *inst, cm_id_t dir, const char *name, size_t nlen, cm_id_t *out)
{
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    uint32_t ino = 0;
    cm_error_t e = dir_find(inst_snap(inst), (uint32_t)dir, name, nlen, &ino, NULL);
    if (e == CM_OK && out) *out = ino;
    return e;
}

static int op_create(void *inst, cm_id_t dir, const char *name, size_t nlen,
                     cm_ino_kind_t kind, uint32_t flags, cm_id_t *out)
{
    if (inst) return CM_ERR_ROFS;
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    if (g_readonly) return CM_ERR_ROFS;
    if (nlen == 0 || nlen > CM_FS_NAME_MAX) return CM_ERR_INVAL;
    if (dir_find(-1, (uint32_t)dir, name, nlen, NULL, NULL) == CM_OK) return CM_ERR_EXIST;
    uint32_t ino = 0;
    for (uint32_t i = FS_ROOT_INO + 1; i < FS_NINODES; i++)
        if (!g_ino[i].ino) { ino = i; break; }
    if (!ino) return CM_ERR_NOSPC;

    cm_cow_txn_begin();
    memset(&g_ino[ino], 0, sizeof(g_ino[ino]));
    g_ino[ino].ino = ino;
    g_ino[ino].kind = (uint16_t)kind;
    g_ino[ino].flags = (uint16_t)(flags & CM_FS_NOCOW);
    g_ino[ino].nlink = 1;
    g_ino[ino].parent = (uint32_t)dir;
    ino_dirty(ino);
    cm_error_t e = dir_add((uint32_t)dir, name, nlen, ino, (uint8_t)kind);
    if (e != CM_OK) {
        memset(&g_ino[ino], 0, sizeof(g_ino[ino]));
        ino_dirty(ino);
    }
    cm_error_t c = cm_cow_txn_end_locked();
    if (e == CM_OK) e = c;
    if (e == CM_OK && out) *out = ino;
    return e;
}

static int64_t op_read(void *inst, cm_id_t ino, uint64_t off, void *buf, uint64_t len)
{
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    return view_read(inst_snap(inst), (uint32_t)ino, off, buf, len);
}

static int64_t op_write(void *inst, cm_id_t ino, uint64_t off, const void *buf, uint64_t len)
{
    if (inst) return CM_ERR_ROFS;
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    cm_cow_txn_begin();
    int64_t w = live_write((uint32_t)ino, off, buf, len);
    cm_error_t c = cm_cow_txn_end_locked();
    return (w >= 0 && c != CM_OK) ? c : w;
}

static int op_truncate(void *inst, cm_id_t ino, uint64_t size)
{
    if (inst) return CM_ERR_ROFS;
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    cm_cow_txn_begin();
    cm_error_t e = live_truncate((uint32_t)ino, size);
    cm_error_t c = cm_cow_txn_end_locked();
    return e != CM_OK ? e : c;
}

static int op_getattr(void *inst, cm_id_t ino, cm_stat_t *st)
{
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    int snap = inst_snap(inst);
    fs_dinode_t di;
    cm_error_t e = view_inode(snap, (uint32_t)ino, &di);
    if (e != CM_OK) return e;
    memset(st, 0, sizeof(*st));
    st->ino = di.ino;
    st->kind = (cm_ino_kind_t)di.kind;
    st->flags = di.flags;
    st->size = di.size;
    st->nlink = di.nlink;
    st->gen = di.gen;
    for (uint32_t d = 0; d < FS_NDIRECT; d++) if (di.d[d].blk) st->blocks++;
    const fs_bptr_t *ind = snap < 0 ? g_ind[di.ino] : NULL;
    if (!ind && di.ind.blk && read_ptr(&di.ind, g_buf_ind, 1) == CM_OK)
        ind = (const fs_bptr_t *)g_buf_ind;
    if (ind) for (uint32_t j = 0; j < FS_NIND; j++) if (ind[j].blk) st->blocks++;
    return CM_OK;
}

static int op_readdir(void *inst, cm_id_t dir, uint32_t idx, cm_dirent_t *out)
{
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    int snap = inst_snap(inst);
    fs_dinode_t di;
    cm_error_t e = view_inode(snap, (uint32_t)dir, &di);
    if (e != CM_OK) return e;
    if (di.kind != CM_INO_DIR) return CM_ERR_NOTDIR;
    uint32_t n = 0;
    fs_dirent_t de;
    for (uint64_t off = 0; off < di.size; off += FS_DIRENT_SZ) {
        int64_t r = view_read(snap, (uint32_t)dir, off, &de, sizeof(de));
        if (r < (int64_t)sizeof(de)) break;
        if (!de.ino) continue;
        if (n++ != idx) continue;
        out->ino = de.ino;
        out->kind = (cm_ino_kind_t)de.kind;
        memcpy(out->name, de.name, de.nlen);
        out->name[de.nlen] = 0;
        return CM_OK;
    }
    return CM_ERR_NOENT;
}

static void free_inode(uint32_t ino)
{
    ind_drop(ino);
    memset(&g_ino[ino], 0, sizeof(g_ino[ino]));
    g_itab_dirty[ino / FS_IPB] = 1;
}

static int op_unlink(void *inst, cm_id_t dir, const char *name, size_t nlen)
{
    if (inst) return CM_ERR_ROFS;
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    if (g_readonly) return CM_ERR_ROFS;
    uint32_t ino = 0;
    uint64_t off = 0;
    cm_error_t e = dir_find(-1, (uint32_t)dir, name, nlen, &ino, &off);
    if (e != CM_OK) return e;
    fs_dinode_t *di = ino_get(ino);
    if (di && di->kind == CM_INO_DIR) {
        cm_dirent_t tmp;
        if (op_readdir(NULL, ino, 0, &tmp) == CM_OK) return CM_ERR_NOTEMPTY;
    }
    cm_cow_txn_begin();
    fs_dirent_t z;
    memset(&z, 0, sizeof(z));
    int64_t w = live_write((uint32_t)dir, off, &z, sizeof(z));
    if (w >= 0 && di && --di->nlink == 0) free_inode(ino);
    else if (di) ino_dirty(ino);
    cm_error_t c = cm_cow_txn_end_locked();
    return w < 0 ? (cm_error_t)w : c;
}

static int op_sync(void *inst)
{
    if (inst) return CM_OK;
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    return g_txn_depth ? CM_OK : commit();
}

static const cm_fs_ops_t g_cowfs_ops = {
    .name = "cowfs",
    .lookup = op_lookup,
    .create = op_create,
    .read = op_read,
    .write = op_write,
    .truncate = op_truncate,
    .getattr = op_getattr,
    .readdir = op_readdir,
    .unlink = op_unlink,
    .sync = op_sync,
    .root = op_root,
};

/* ---- mount / mkfs ----------------------------------------------------- */

static void mount_snaps(void)
{
    for (uint32_t s = 0; s < g_sb.nsnaps; s++) {
        char at[32] = "/.snap/";
        size_t n = strlen(g_sb.snaps[s].name);
        memcpy(at + 7, g_sb.snaps[s].name, n);
        at[7 + n] = 0;
        cm_vfs_mount(at, &g_cowfs_ops, (void *)(uintptr_t)(s + 1));
    }
}

static void umount_snaps(void)
{
    for (uint32_t s = 0; s < CM_MAX_SNAPS; s++) {
        if (s >= g_sb.nsnaps) break;
        char at[32] = "/.snap/";
        size_t n = strlen(g_sb.snaps[s].name);
        memcpy(at + 7, g_sb.snaps[s].name, n);
        at[7 + n] = 0;
        cm_vfs_umount(at);
    }
}

static int read_super(uint32_t slot, fs_super_t *out)
{
    uint8_t sec[CM_BLK_SECTOR];
    if (cm_blk_rw(CM_OP_READ, cm_cow_fs_lba(slot), 1, sec) != CM_OK) return 0;
    memcpy(out, sec, sizeof(*out));
    return out->magic == FS_MAGIC && out->nblocks == FS_NBLK &&
           out->ninodes == FS_NINODES && out->nsnaps <= CM_MAX_SNAPS &&
           out->csum == sb_csum(out);
}

static void reset_memory(void)
{
    for (uint32_t i = 0; i < FS_NINODES; i++) ind_drop(i);
    memset(g_ino, 0, sizeof(g_ino));
    memset(g_omap, 0, sizeof(g_omap));
    memset(g_itab_dirty, 0, sizeof(g_itab_dirty));
    memset(g_pend, 0, sizeof(g_pend));
    memset(g_shared, 0, sizeof(g_shared));
    g_sb_dirty = 0;
    g_txn_depth = 0;
    g_crash_point = CM_CRASH_NONE;
    g_readonly = 0;
    /* Anything cached may describe a block the crashed transaction wrote
     * and the mounted tree does not reference. */
    cm_pcache_invalidate_all();
}

static cm_error_t cm_cow_mount_locked(void)
{
    if (g_mounted) umount_snaps();
    g_mounted = 0;
    reset_memory();

    fs_super_t a, b;
    int va = read_super(0, &a), vb = read_super(1, &b);
    if (!va && !vb) return CM_ERR_NOENT;
    g_sb = (va && (!vb || a.gen > b.gen)) ? a : b;

    cm_error_t e = read_ptr(&g_sb.omap, g_buf_omap, 1);
    if (e != CM_OK) { kerr("cowfs: object map unreadable"); return e; }
    memcpy(g_omap, g_buf_omap, sizeof(g_omap));
    for (uint32_t t = 0; t < FS_NITAB; t++) {
        if (!g_omap[t].blk) continue;
        if ((e = read_ptr(&g_omap[t], g_buf_itab, 1)) != CM_OK) {
            kerr("cowfs: inode table block %u unreadable", t);
            return e;
        }
        memcpy(&g_ino[t * FS_IPB], g_buf_itab, FS_BS);
    }
    g_crashed = 0;
    g_mounted = 1;
    recount();
    mount_snaps();
    kinfo("cowfs: mounted gen=%llu (slot %c, other slot %s) snaps=%u",
          (unsigned long)g_sb.gen, (g_sb.gen & 1) ? 'B' : 'A',
          (va && vb) ? "valid/older" : "invalid", g_sb.nsnaps);
    return CM_OK;
}

static cm_error_t cm_cow_mkfs_locked(void)
{
    if (g_mounted) umount_snaps();
    g_mounted = 0;
    reset_memory();
    /* Kill both superblock slots first so a half-done mkfs never mounts. */
    uint8_t z[CM_BLK_SECTOR];
    memset(z, 0, sizeof(z));
    cm_blk_rw(CM_OP_WRITE, cm_cow_fs_lba(0), 1, z);
    cm_blk_rw(CM_OP_WRITE, cm_cow_fs_lba(1), 1, z);

    memset(&g_sb, 0, sizeof(g_sb));
    g_sb.magic = FS_MAGIC;
    g_sb.gen = 0;
    g_sb.nblocks = FS_NBLK;
    g_sb.ninodes = FS_NINODES;
    memset(g_ref, 0, sizeof(g_ref));
    g_mounted = 1;
    g_crashed = 0;

    g_ino[FS_ROOT_INO].ino = FS_ROOT_INO;
    g_ino[FS_ROOT_INO].kind = CM_INO_DIR;
    g_ino[FS_ROOT_INO].nlink = 2;
    g_ino[FS_ROOT_INO].parent = FS_ROOT_INO;
    ino_dirty(FS_ROOT_INO);
    cm_error_t e = commit();
    if (e != CM_OK) { g_mounted = 0; return e; }
    kinfo("cowfs: mkfs %u blocks x %u B, %u inodes, %d AGs, gen=%llu",
          FS_NBLK, FS_BS, FS_NINODES, CM_MAX_AG, (unsigned long)g_sb.gen);
    return CM_OK;
}

const cm_fs_ops_t *cm_cowfs_ops(void) { return &g_cowfs_ops; }

/* ---- snapshots / clones / scrub --------------------------------------- */

static int snap_find(const char *name)
{
    for (uint32_t s = 0; s < g_sb.nsnaps; s++)
        if (strlen(g_sb.snaps[s].name) == strlen(name) &&
            memcmp(g_sb.snaps[s].name, name, strlen(name)) == 0)
            return (int)s;
    return -1;
}

static cm_error_t cm_cow_snapshot_locked(const char *name)
{
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    if (!name || !*name || strlen(name) >= sizeof(g_sb.snaps[0].name)) return CM_ERR_INVAL;
    if (g_sb.nsnaps >= CM_MAX_SNAPS) return CM_ERR_NOSPC;
    if (snap_find(name) >= 0) return CM_ERR_EXIST;
    if (g_txn_depth) return CM_ERR_BUSY;
    /* A snapshot is a name for the committed tree: flush first, then the
     * snapshot shares every block with the live tree (zero copy). */
    cm_error_t e = commit();
    if (e != CM_OK) return e;
    umount_snaps();
    fs_snap_t *s = &g_sb.snaps[g_sb.nsnaps];
    memset(s, 0, sizeof(*s));
    s->gen = g_sb.gen;
    s->ctime = cm_ticks_now();
    memcpy(s->name, name, strlen(name));
    s->omap = g_sb.omap;
    g_sb.nsnaps++;
    g_sb_dirty = 1;
    e = commit();
    mount_snaps();
    if (e == CM_OK)
        kinfo("cowfs: snapshot '%s' = gen %llu (zero-copy, read-only at /.snap/%s)",
              name, (unsigned long)s->gen, name);
    return e;
}

static cm_error_t cm_cow_snapshot_delete_locked(const char *name)
{
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    int s = name ? snap_find(name) : -1;
    if (s < 0) return CM_ERR_NOENT;
    if (g_txn_depth) return CM_ERR_BUSY;
    umount_snaps();
    for (uint32_t i = (uint32_t)s; i + 1 < g_sb.nsnaps; i++) g_sb.snaps[i] = g_sb.snaps[i + 1];
    g_sb.nsnaps--;
    memset(&g_sb.snaps[g_sb.nsnaps], 0, sizeof(fs_snap_t));
    g_sb_dirty = 1;
    cm_error_t e = commit();
    mount_snaps();
    return e;
}

/* Clone = new inode with the SAME block pointers (APFS clonefile / ReFS
 * block cloning). Data is not copied; the first write to either side
 * goes to a new block because cow writes always do, and a nocow write
 * checks g_shared before daring to overwrite in place. */
cm_error_t cm_cowfs_clone_ino(cm_id_t src, cm_id_t dstdir, const char *name, size_t nlen,
                              cm_id_t *out)
{
    fs_dinode_t *s = ino_get(src);
    if (!s) return CM_ERR_NOENT;
    if (s->kind != CM_INO_FILE) return CM_ERR_ISDIR;
    if (s->ind.blk && ind_load(s, 0) != CM_OK) return CM_ERR_IO;
    cm_id_t ino = 0;
    cm_cow_txn_begin();
    cm_error_t e = op_create(NULL, dstdir, name, nlen, CM_INO_FILE, s->flags, &ino);
    if (e == CM_OK) {
        fs_dinode_t *d = &g_ino[ino];
        d->size = s->size;
        memcpy(d->d, s->d, sizeof(d->d));
        for (uint32_t i = 0; i < FS_NDIRECT; i++) if (s->d[i].blk) g_shared[s->d[i].blk] = 1;
        if (g_ind[src]) {
            if (ind_load(d, 1) == CM_OK) {
                memcpy(g_ind[ino], g_ind[src], FS_BS);
                g_ind_dirty[ino] = 1;
                for (uint32_t j = 0; j < FS_NIND; j++)
                    if (g_ind[src][j].blk) g_shared[g_ind[src][j].blk] = 1;
            } else {
                e = CM_ERR_NOMEM;
            }
        }
        ino_dirty((uint32_t)ino);
    }
    cm_error_t c = cm_cow_txn_end_locked();
    if (e == CM_OK) e = c;
    if (e == CM_OK && out) *out = ino;
    return e;
}

static cm_error_t cm_cow_scrub_locked(cm_scrub_report_t *out)
{
    if (live_ok() != CM_OK) return CM_ERR_DEAD;
    if (g_txn_depth || any_dirty()) {
        cm_error_t e = commit();
        if (e != CM_OK) return e;
    }
    static uint8_t seen[FS_NBLK];
    memset(seen, 0, sizeof(seen));
    cm_scrub_report_t rep;
    memset(&rep, 0, sizeof(rep));
    walk_t w = { 1, &rep, seen, 0 };
    walk_all(&w);
    if (out) *out = rep;
    kinfo("cowfs: scrub meta=%llu (repaired %llu, lost %llu) data=%llu (errors %llu)",
          (unsigned long)rep.meta_checked, (unsigned long)rep.meta_repaired,
          (unsigned long)rep.meta_lost, (unsigned long)rep.data_checked,
          (unsigned long)rep.data_errors);
    return (rep.meta_lost || rep.data_errors) ? CM_ERR_IO : CM_OK;
}

void cm_cow_volstat(cm_vol_stat_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->super_gen = g_sb.gen;
    out->nsnaps = g_sb.nsnaps;
    out->total_blocks = FS_NBLK;
    for (uint32_t b = 0; b < FS_NBLK; b++) {
        if (!blk_busy(b)) continue;
        out->used_blocks++;
        out->ag_used[ag_of(b)]++;
    }
    out->free_blocks = FS_NBLK - out->used_blocks;
    for (uint32_t i = 1; i < FS_NINODES; i++) if (g_ino[i].ino) out->files++;
    out->commits = g_commits;
    out->cow_writes = g_cow_writes;
    out->inplace_writes = g_inplace;
    out->csum_errors = g_csum_err;
    out->read_repairs = g_read_repair;
    out->readonly = g_readonly;
}

uint64_t cm_cow_super_gen(void) { return g_sb.gen; }

uint32_t cm_cowfs_bmap_ino(cm_id_t ino, uint64_t off)
{
    fs_dinode_t *di = ino_get(ino);
    if (!di) return 0;
    uint64_t fb = off / FS_BS;
    if (fb < FS_NDIRECT) return di->d[fb].blk;
    if (fb >= FS_MAXFBLK || ind_load(di, 0) != CM_OK) return 0;
    return g_ind[ino][fb - FS_NDIRECT].blk;
}

/* Test hook: where the live inode-table block 0 lives (both copies), so a
 * selftest can rot exactly one metadata copy on the raw device. */
void cm_cow_debug_itab0(uint32_t *a, uint32_t *b)
{
    if (a) *a = g_omap[0].blk;
    if (b) *b = g_omap[0].dup;
}

cm_error_t cm_cowfs_init(void)
{
    g_mounted = 0;
    g_crashed = 0;
    if (cm_cow_mount_locked() == CM_OK) return CM_OK;
    kinfo("cowfs: no valid superblock — formatting");
    return cm_cow_mkfs_locked();
}

cm_error_t cm_cow_snapshot(const char *name)
{
    cm_vfs_lock();
    cm_error_t e = cm_cow_snapshot_locked(name);
    cm_vfs_unlock();
    return e;
}

cm_error_t cm_cow_snapshot_delete(const char *name)
{
    cm_vfs_lock();
    cm_error_t e = cm_cow_snapshot_delete_locked(name);
    cm_vfs_unlock();
    return e;
}

cm_error_t cm_cow_scrub(cm_scrub_report_t *out)
{
    cm_vfs_lock();
    cm_error_t e = cm_cow_scrub_locked(out);
    cm_vfs_unlock();
    return e;
}

cm_error_t cm_cow_mount(void)
{
    cm_vfs_lock();
    cm_error_t e = cm_cow_mount_locked();
    cm_vfs_unlock();
    return e;
}

cm_error_t cm_cow_mkfs(void)
{
    cm_vfs_lock();
    cm_error_t e = cm_cow_mkfs_locked();
    cm_vfs_unlock();
    return e;
}

cm_error_t cm_cow_txn_end(void)
{
    cm_vfs_lock();
    cm_error_t e = cm_cow_txn_end_locked();
    cm_vfs_unlock();
    return e;
}
