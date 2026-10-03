#ifndef CAPMIX_FS_H
#define CAPMIX_FS_H

#include "types.h"

/* Layer 4 — COW school (APFS blueprint + Btrfs checksum/compress
 * + XFS AG + ReFS scrubber/sparse VDL). Journaling is rejected.
 * nocow attr is mandatory for DB / VM-image write amplification.
 */

#define CM_FS_NOCOW      (1u << 0)
#define CM_FS_COMPRESS   (1u << 1)
#define CM_MAX_AG        4
#define CM_VOL_BLOCKS    512
#define CM_MAX_INODES    64
#define CM_MAX_EXTENTS   8
#define CM_MAX_SNAPS     8

typedef enum {
    CM_INO_FILE = 1,
    CM_INO_DIR,
    CM_INO_SYMLINK,
} cm_ino_kind_t;

typedef struct {
    uint32_t blk;
    uint16_t nblk;
    uint16_t gen;
} cm_extent_t;

typedef struct {
    cm_id_t       ino;
    cm_ino_kind_t kind;
    uint32_t      flags;
    uint64_t      size;
    uint64_t      csum;
    uint64_t      cow_gen;
    uint32_t      refs;          /* clone share count */
    uint32_t      nextents;
    cm_extent_t   ext[CM_MAX_EXTENTS];
} cm_inode_t;

typedef struct {
    uint64_t super_gen;          /* atomic pointer flip target */
    uint64_t nsnaps;
    uint64_t used_blocks;
    uint64_t free_blocks;
    uint64_t files;
    uint32_t last_scrub_errors;
} cm_vol_stat_t;

cm_error_t cm_vfs_init(void);
cm_error_t cm_cow_snapshot(const char *vol);
cm_error_t cm_cow_clone(cm_id_t ino, cm_id_t *out_clone);

cm_error_t cm_vfs_create(const char *path, uint32_t flags, cm_id_t *out_ino);
cm_error_t cm_vfs_write(cm_id_t ino, uint64_t off, const void *buf, uint64_t len);
cm_error_t cm_vfs_read(cm_id_t ino, uint64_t off, void *buf, uint64_t len);
cm_error_t cm_vfs_getattr(cm_id_t ino, cm_inode_t *out);
cm_error_t cm_vfs_scrub(void);
void       cm_vfs_stat(cm_vol_stat_t *out);
uint64_t   cm_vfs_super_gen(void);

#endif
