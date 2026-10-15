#ifndef CAPMIX_FS_H
#define CAPMIX_FS_H

#include "types.h"

/* Layer 4 — COW school (APFS blueprint + Btrfs checksum + XFS AG + ReFS
 * scrubber/sparse VDL). Journaling is rejected; nocow is the escape hatch.
 *
 * On disk (fs partition of the blk device, 4 KiB blocks):
 *   block 0/1   superblock slots A/B — a commit writes the OTHER slot, and
 *               mount takes the valid one with the higher generation. That
 *               single sector write is the atomic pointer flip.
 *   omap        object map: pointers to the inode-table blocks
 *   itab        inode table blocks (16 x 256 B inodes each)
 *   ind         single-indirect pointer blocks
 *   data        file / directory content
 * Every pointer carries the checksum of the block it points at (Merkle
 * path from the superblock down). Metadata (omap/itab/ind) is written
 * twice (DUP) so scrub and read-repair can heal one bad copy; data is
 * single-copy, so damage there is DETECTED (EIO, scrub) but not repaired.
 * Nothing is ever overwritten in place except nocow file data. */

#define CM_FS_NOCOW      (1u << 0)
#define CM_FS_COMPRESS   (1u << 1)    /* reserved, not implemented */
#define CM_MAX_AG        4
#define CM_MAX_SNAPS     8
#define CM_FS_NAME_MAX   57

/* open() flags — POSIX-shaped, routed through fdio (conflict #3). */
#define CM_O_RDONLY  0x0
#define CM_O_WRONLY  0x1
#define CM_O_RDWR    0x2
#define CM_O_ACCMODE 0x3
#define CM_O_CREAT   0x40
#define CM_O_TRUNC   0x200
#define CM_O_APPEND  0x400
#define CM_O_NOCOW   0x100000         /* creation attribute (Btrfs chattr +C) */

typedef enum {
    CM_INO_FILE = 1,
    CM_INO_DIR,
} cm_ino_kind_t;

typedef struct {
    cm_id_t       ino;
    cm_ino_kind_t kind;
    uint32_t      flags;
    uint64_t      size;
    uint64_t      blocks;       /* allocated (non-hole) blocks */
    uint32_t      nlink;
    uint64_t      gen;          /* txn that last changed it */
} cm_stat_t;

typedef struct {
    cm_id_t       ino;
    cm_ino_kind_t kind;
    char          name[CM_FS_NAME_MAX + 1];
} cm_dirent_t;

typedef struct {
    uint64_t super_gen;          /* atomic pointer flip target */
    uint64_t nsnaps;
    uint64_t used_blocks;
    uint64_t free_blocks;
    uint64_t total_blocks;
    uint64_t files;
    uint64_t commits;
    uint64_t cow_writes;         /* data blocks written to a NEW location */
    uint64_t inplace_writes;     /* nocow overwrites */
    uint64_t csum_errors;        /* detected on read */
    uint64_t read_repairs;       /* bad metadata copy healed on read */
    uint32_t ag_used[CM_MAX_AG];
    int      readonly;           /* degraded: unrecoverable metadata */
} cm_vol_stat_t;

typedef struct {
    uint64_t meta_checked;
    uint64_t meta_repaired;
    uint64_t meta_lost;
    uint64_t data_checked;
    uint64_t data_errors;
} cm_scrub_report_t;

/* Pluggable filesystem — one ops table per FS type, one inst per mount. */
typedef struct cm_fs_ops {
    const char *name;
    int      (*lookup)(void *inst, cm_id_t dir, const char *name, size_t nlen, cm_id_t *out);
    int      (*create)(void *inst, cm_id_t dir, const char *name, size_t nlen,
                       cm_ino_kind_t kind, uint32_t flags, cm_id_t *out);
    int64_t  (*read)(void *inst, cm_id_t ino, uint64_t off, void *buf, uint64_t len);
    int64_t  (*write)(void *inst, cm_id_t ino, uint64_t off, const void *buf, uint64_t len);
    int      (*truncate)(void *inst, cm_id_t ino, uint64_t size);
    int      (*getattr)(void *inst, cm_id_t ino, cm_stat_t *st);
    int      (*readdir)(void *inst, cm_id_t dir, uint32_t idx, cm_dirent_t *out);
    int      (*unlink)(void *inst, cm_id_t dir, const char *name, size_t nlen);
    int      (*sync)(void *inst);
    cm_id_t  (*root)(void *inst);
} cm_fs_ops_t;

typedef struct cm_file cm_file_t;

/* VFS (fs/vfs.c). Paths are absolute. Mounts: "/" = live COW volume,
 * "/.snap/<name>" = each snapshot, read-only. */
cm_error_t cm_vfs_init(void);
void       cm_vfs_lock(void);     /* recursive; serialises FS entry */
void       cm_vfs_unlock(void);
cm_error_t cm_vfs_mount(const char *at, const cm_fs_ops_t *ops, void *inst);
cm_error_t cm_vfs_umount(const char *at);

cm_error_t cm_vfs_open(const char *path, uint32_t oflags, cm_file_t **out);
int64_t    cm_vfs_read(cm_file_t *f, void *buf, uint64_t len);
int64_t    cm_vfs_write(cm_file_t *f, const void *buf, uint64_t len);
int64_t    cm_vfs_pread(cm_file_t *f, void *buf, uint64_t len, uint64_t off);
int64_t    cm_vfs_pwrite(cm_file_t *f, const void *buf, uint64_t len, uint64_t off);
cm_error_t cm_vfs_fsync(cm_file_t *f);
cm_error_t cm_vfs_close(cm_file_t *f);
cm_error_t cm_vfs_fstat(cm_file_t *,cm_stat_t *);
int64_t cm_vfs_seek(cm_file_t *,int64_t,int);
cm_error_t cm_vfs_nextdir(cm_file_t *,cm_dirent_t *,uint64_t *);
cm_error_t cm_vfs_file_path(cm_file_t *,char *,size_t);
uint32_t   cm_vfs_file_mode(const cm_file_t *f);

cm_error_t cm_vfs_stat(const char *path, cm_stat_t *out);
cm_error_t cm_vfs_mkdir(const char *path);
cm_error_t cm_vfs_unlink(const char *path);
cm_error_t cm_vfs_readdir(const char *path, uint32_t idx, cm_dirent_t *out);
cm_error_t cm_vfs_sync(void);
uint32_t   cm_vfs_open_files(void);

/* COW volume specifics (fs/cowfs.c). */
cm_error_t cm_cow_snapshot(const char *name);
cm_error_t cm_cow_snapshot_delete(const char *name);
cm_error_t cm_cow_clone(const char *src, const char *dst);
cm_error_t cm_cow_scrub(cm_scrub_report_t *out);
void       cm_cow_volstat(cm_vol_stat_t *out);
uint64_t   cm_cow_super_gen(void);
void       cm_cow_txn_begin(void);
cm_error_t cm_cow_txn_end(void);
cm_error_t cm_cow_mount(void);     /* drop ALL in-memory state, remount from disk */
cm_error_t cm_cow_mkfs(void);
/* Block number (fs-relative) currently backing byte `off` of a live file,
 * 0 for a hole. Lets tests prove COW moved a block / nocow did not. */
uint32_t   cm_cow_bmap(const char *path, uint64_t off);
uint64_t   cm_cow_fs_lba(uint32_t blk);   /* blk -> device LBA */

/* Crash injection: the NEXT commit stops at this point as if power were cut.
 * In-memory state is then unusable until cm_cow_mount(). */
#define CM_CRASH_NONE        0
#define CM_CRASH_AFTER_DATA  1   /* data blocks written, no metadata */
#define CM_CRASH_AFTER_ITAB  2   /* + inode table blocks */
#define CM_CRASH_AFTER_OMAP  3   /* + object map, superblock not written */
#define CM_CRASH_TORN_SUPER  4   /* superblock sector half-written */
void       cm_cow_inject_crash(int point);
void       cm_cow_debug_itab0(uint32_t *a, uint32_t *b);

#endif
