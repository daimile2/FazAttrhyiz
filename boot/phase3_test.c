#include "capmix/kernel.h"
#include "capmix/fs.h"
#include "capmix/io.h"
#include "capmix/mm.h"
#include "capmix/proc.h"
#include "capmix/syscall.h"
#include "capmix/arch.h"
#include "capmix/numa.h"

#include <string.h>

/* Phase 3 acceptance: "memory pressure pipeline + VFS + a COW filesystem —
 * fill memory, do not die, do not stall".
 *
 * Every check here goes through public interfaces, and every durability
 * claim is checked the hard way: drop ALL in-memory FS state and remount
 * from the raw device (cm_cow_mount), or corrupt the raw device underneath
 * the FS (cm_blk_raw_sector). Nothing is taken on the FS's word. */

static int g_f;

#define CHECK(cond, ...) do {                                   \
        if (!(cond)) { kerr("p3: FAIL " __VA_ARGS__); g_f++; }  \
    } while (0)

static uint8_t g_wbuf[128 * 1024];
static uint8_t g_rbuf[128 * 1024];

static void fill_pattern(uint8_t *b, uint64_t n, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    for (uint64_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        b[i] = (uint8_t)x;
    }
}

static int64_t put(const char *path, uint32_t fl, const void *buf, uint64_t n, uint64_t off)
{
    cm_file_t *f = NULL;
    cm_error_t e = cm_vfs_open(path, CM_O_RDWR | CM_O_CREAT | fl, &f);
    if (e != CM_OK) return e;
    int64_t w = cm_vfs_pwrite(f, buf, n, off);
    cm_vfs_close(f);
    return w;
}

static int64_t get(const char *path, void *buf, uint64_t n, uint64_t off)
{
    cm_file_t *f = NULL;
    cm_error_t e = cm_vfs_open(path, CM_O_RDONLY, &f);
    if (e != CM_OK) return e;
    int64_t r = cm_vfs_pread(f, buf, n, off);
    cm_vfs_close(f);
    return r;
}

static int same(const char *path, const char *want)
{
    char b[64];
    memset(b, 0, sizeof(b));
    size_t n = strlen(want);
    return get(path, b, n, 0) == (int64_t)n && memcmp(b, want, n) == 0;
}

/* ---- 1. VFS ------------------------------------------------------------ */

static void t_vfs(void)
{
    kinfo("--- p3: VFS (namespace, multi-block I/O, persistence) ---");
    CHECK(cm_vfs_mkdir("/data") == CM_OK, "mkdir /data");
    CHECK(cm_vfs_mkdir("/data/sub") == CM_OK, "mkdir /data/sub");
    CHECK(cm_vfs_mkdir("/data") == CM_ERR_EXIST, "mkdir twice must be EXIST");

    /* 100 KiB: 12 direct blocks + the single-indirect level. */
    const uint64_t big = 100 * 1024 + 123;
    fill_pattern(g_wbuf, big, 7);
    CHECK(put("/data/big.bin", 0, g_wbuf, big, 0) == (int64_t)big, "write 100 KiB");
    memset(g_rbuf, 0, big);
    CHECK(get("/data/big.bin", g_rbuf, big, 0) == (int64_t)big &&
          memcmp(g_rbuf, g_wbuf, big) == 0, "read back 100 KiB");
    cm_stat_t st;
    CHECK(cm_vfs_stat("/data/big.bin", &st) == CM_OK && st.size == big &&
          st.blocks == (big + 4095) / 4096, "stat size/blocks");

    CHECK(put("/data/sub/x", 0, "x", 1, 0) == 1, "create nested");
    cm_dirent_t de;
    int n = 0, saw_big = 0, saw_sub = 0;
    for (uint32_t i = 0; cm_vfs_readdir("/data", i, &de) == CM_OK; i++, n++) {
        if (!memcmp(de.name, "big.bin", 8)) saw_big = 1;
        if (!memcmp(de.name, "sub", 4) && de.kind == CM_INO_DIR) saw_sub = 1;
    }
    CHECK(n == 2 && saw_big && saw_sub, "readdir /data (n=%d)", n);

    cm_file_t *f = NULL;
    CHECK(cm_vfs_open("/nope", CM_O_RDONLY, &f) == CM_ERR_NOENT, "open missing -> NOENT");
    CHECK(cm_vfs_open("/data/big.bin/x", CM_O_RDONLY, &f) == CM_ERR_NOTDIR,
          "walk through a file -> NOTDIR");
    CHECK(cm_vfs_unlink("/data/sub") == CM_ERR_NOTEMPTY, "rmdir non-empty");
    CHECK(cm_vfs_unlink("/data/sub/x") == CM_OK && cm_vfs_unlink("/data/sub") == CM_OK,
          "unlink file then dir");
    CHECK(cm_vfs_stat("/data/sub", &st) == CM_ERR_NOENT, "gone after unlink");

    /* O_TRUNC + O_APPEND */
    CHECK(put("/data/log", 0, "abc", 3, 0) == 3, "log create");
    CHECK(cm_vfs_open("/data/log", CM_O_WRONLY | CM_O_APPEND, &f) == CM_OK &&
          cm_vfs_write(f, "def", 3) == 3 && cm_vfs_close(f) == CM_OK, "append");
    CHECK(same("/data/log", "abcdef"), "append result");
    CHECK(cm_vfs_open("/data/log", CM_O_WRONLY | CM_O_TRUNC, &f) == CM_OK &&
          cm_vfs_close(f) == CM_OK && cm_vfs_stat("/data/log", &st) == CM_OK && st.size == 0,
          "O_TRUNC -> size 0");

    /* Durability: forget everything in RAM, mount from the device. */
    uint64_t gen = cm_cow_super_gen();
    CHECK(cm_cow_mount() == CM_OK, "remount");
    CHECK(cm_cow_super_gen() == gen, "remount keeps generation");
    memset(g_rbuf, 0, big);
    CHECK(get("/data/big.bin", g_rbuf, big, 0) == (int64_t)big &&
          memcmp(g_rbuf, g_wbuf, big) == 0, "100 KiB intact after remount");
    kinfo("p3: vfs ok — 100 KiB file via indirect block, readdir/unlink/trunc/append, "
          "survives remount at gen %llu", (unsigned long)gen);
}

/* ---- 2. syscalls + capability rights ----------------------------------- */

static volatile int64_t g_sc[8];

static void p3_file_user(void)
{
    char buf[32];
    memset(buf, 0, sizeof(buf));
    g_sc[0] = cm_syscall(CM_SYS_MKDIR, (uint64_t)(uintptr_t)"/home", 0, 0, 0);
    int64_t fd = cm_syscall(CM_SYS_OPEN, (uint64_t)(uintptr_t)"/home/u.txt",
                            CM_O_RDWR | CM_O_CREAT, 0, 0);
    g_sc[1] = fd;
    g_sc[2] = cm_syscall(CM_SYS_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)"syscall-io", 10, 0);
    g_sc[3] = cm_syscall(CM_SYS_FSYNC, (uint64_t)fd, 0, 0, 0);
    cm_syscall(CM_SYS_CLOSE, (uint64_t)fd, 0, 0, 0);
    fd = cm_syscall(CM_SYS_OPEN, (uint64_t)(uintptr_t)"/home/u.txt", CM_O_RDONLY, 0, 0);
    g_sc[4] = cm_syscall(CM_SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)buf, sizeof(buf), 0);
    g_sc[5] = memcmp(buf, "syscall-io", 10) == 0;
    /* The handle behind a read-only fd has no WRITE right. */
    g_sc[6] = cm_syscall(CM_SYS_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)"x", 1, 0);
    g_sc[7] = cm_syscall(CM_SYS_CLOSE, (uint64_t)fd, 0, 0, 0);
    cm_syscall(CM_SYS_EXIT, 0, 0, 0, 0);
}

static void t_syscalls(void)
{
    kinfo("--- p3: file syscalls through fdio -> capability handles ---");
    uint32_t open0 = cm_vfs_open_files();
    cm_spawn_attr_t a = { .path = "/bin/filetest", .entry = p3_file_user,
                          .qos = CM_QOS_USER_INITIATED, .image_pages = 4, .stack_pages = 2 };
    cm_id_t pid = 0;
    if (cm_posix_spawn(&a, &pid) != CM_OK) { kerr("p3: spawn filetest"); g_f++; return; }
    cm_sched_run_until_idle(64);
    cm_proc_t *p = cm_proc_lookup(pid);
    CHECK(p && p->exited, "filetest ran to exit");
    CHECK(g_sc[0] == CM_OK, "sys mkdir (%lld)", (long)g_sc[0]);
    CHECK(g_sc[1] > 2, "sys open -> fd (%lld)", (long)g_sc[1]);
    CHECK(g_sc[2] == 10 && g_sc[3] == CM_OK, "sys write/fsync");
    CHECK(g_sc[4] == 10 && g_sc[5], "sys read back");
    CHECK(g_sc[6] == CM_ERR_DENIED, "write via read-only handle must be DENIED (%lld)",
          (long)g_sc[6]);
    CHECK(g_sc[7] == 0, "sys close");
    CHECK(cm_vfs_open_files() == open0, "no leaked open files (%u vs %u)",
          cm_vfs_open_files(), open0);
    CHECK(same("/home/u.txt", "syscall-io"), "kernel sees what user wrote");
    kinfo("p3: syscalls ok — open/read/write/fsync/close, read-only handle refused write");
}

/* ---- 3. COW semantics -------------------------------------------------- */

static void t_cow(void)
{
    kinfo("--- p3: COW (pointer flip, snapshot, clone, nocow, sparse) ---");
    CHECK(put("/data/note", CM_O_TRUNC, "generation-one", 14, 0) == 14, "note v1");
    uint32_t b0 = cm_cow_bmap("/data/note", 0);
    uint64_t g0 = cm_cow_super_gen();
    CHECK(put("/data/note", 0, "generation-ONE", 14, 0) == 14, "note overwrite");
    uint32_t b1 = cm_cow_bmap("/data/note", 0);
    CHECK(b0 && b1 && b0 != b1, "overwrite moved block %u -> %u (COW)", b0, b1);
    CHECK(cm_cow_super_gen() > g0, "superblock generation advanced");

    CHECK(cm_cow_snapshot("s1") == CM_OK, "snapshot s1");
    CHECK(put("/data/note", 0, "generation-two", 14, 0) == 14, "note v2");
    CHECK(same("/.snap/s1/data/note", "generation-ONE"), "snapshot keeps old content");
    CHECK(same("/data/note", "generation-two"), "live has new content");
    cm_file_t *f = NULL;
    CHECK(cm_vfs_open("/.snap/s1/data/note", CM_O_RDWR, &f) == CM_ERR_ROFS ||
          (f && cm_vfs_write(f, "z", 1) == CM_ERR_ROFS && cm_vfs_close(f) == CM_OK),
          "snapshot is read-only");

    CHECK(cm_cow_clone("/data/note", "/data/note.clone") == CM_OK, "clone");
    CHECK(cm_cow_bmap("/data/note.clone", 0) == cm_cow_bmap("/data/note", 0),
          "clone shares the block (zero copy)");
    CHECK(put("/data/note.clone", 0, "clone-changed!", 14, 0) == 14, "write clone");
    CHECK(cm_cow_bmap("/data/note.clone", 0) != cm_cow_bmap("/data/note", 0),
          "clone write got its own block");
    CHECK(same("/data/note", "generation-two") && same("/data/note.clone", "clone-changed!"),
          "origin and clone independent");

    /* nocow: in place while unshared, COW once a snapshot shares it. */
    CHECK(put("/data/db", CM_O_NOCOW, "row-1", 5, 0) == 5, "nocow create");
    cm_cow_txn_begin(); cm_cow_txn_end();
    uint32_t n0 = cm_cow_bmap("/data/db", 0);
    CHECK(put("/data/db", 0, "row-2", 5, 0) == 5, "nocow overwrite");
    CHECK(cm_cow_bmap("/data/db", 0) == n0, "nocow overwrote in place (blk %u)", n0);
    CHECK(cm_cow_snapshot("s2") == CM_OK, "snapshot s2");
    CHECK(put("/data/db", 0, "row-3", 5, 0) == 5, "nocow write after snapshot");
    CHECK(cm_cow_bmap("/data/db", 0) != n0, "nocow block shared by snapshot -> COWed");
    CHECK(same("/.snap/s2/data/db", "row-2") && same("/data/db", "row-3"),
          "snapshot of nocow file not rewritten");

    /* Sparse: one byte at 200 KiB allocates one block, the hole reads 0. */
    CHECK(put("/data/sparse", 0, "!", 1, 200 * 1024) == 1, "sparse write");
    cm_stat_t st;
    CHECK(cm_vfs_stat("/data/sparse", &st) == CM_OK && st.size == 200 * 1024 + 1 &&
          st.blocks == 1, "sparse: size %llu, %llu block(s)",
          (unsigned long)st.size, (unsigned long)st.blocks);
    uint8_t hole[64];
    memset(hole, 0xEE, sizeof(hole));
    int zero = get("/data/sparse", hole, sizeof(hole), 100 * 1024) == (int64_t)sizeof(hole);
    for (size_t i = 0; i < sizeof(hole); i++) zero &= hole[i] == 0;
    CHECK(zero, "hole reads as zeros");

    /* Deleting snapshots returns their exclusive blocks. */
    cm_vol_stat_t v0, v1;
    cm_cow_volstat(&v0);
    CHECK(cm_cow_snapshot_delete("s1") == CM_OK && cm_cow_snapshot_delete("s2") == CM_OK,
          "delete snapshots");
    cm_cow_volstat(&v1);
    CHECK(v1.free_blocks > v0.free_blocks && v1.nsnaps == 0,
          "snapshot delete freed %llu blocks",
          (unsigned long)(v1.free_blocks - v0.free_blocks));
    CHECK(cm_vfs_stat("/.snap/s1/data/note", &st) != CM_OK, "deleted snapshot unmounted");
    kinfo("p3: cow ok — moved blocks, snapshot read-only+zero-copy, clone, nocow in-place "
          "until shared, sparse holes, snapshot delete frees %llu blocks",
          (unsigned long)(v1.free_blocks - v0.free_blocks));
}

/* ---- 4. crash consistency ---------------------------------------------- */

static void t_crash(void)
{
    kinfo("--- p3: crash at every commit stage, remount from disk ---");
    static const char *names[] = { "", "after-data", "after-itab", "after-omap", "torn-super" };
    cm_vfs_mkdir("/crash");
    for (int pt = CM_CRASH_AFTER_DATA; pt <= CM_CRASH_TORN_SUPER; pt++) {
        char old[16] = "OLD-x", nw[16] = "NEW-x", extra[24] = "/crash/new-x";
        old[4] = nw[4] = extra[11] = (char)('0' + pt);
        CHECK(put("/crash/f", CM_O_TRUNC, old, 5, 0) == 5, "baseline %d", pt);
        cm_vol_stat_t before;
        cm_cow_volstat(&before);

        cm_cow_txn_begin();
        put("/crash/f", 0, nw, 5, 0);
        put(extra, 0, "zz", 2, 0);
        for (int k = 0; k < 4; k++) put("/data/big.bin", 0, nw, 5, (uint64_t)k * 40000);
        cm_cow_inject_crash(pt);
        cm_error_t e = cm_cow_txn_end();
        CHECK(e == CM_ERR_DEAD, "commit should die at %s (%d)", names[pt], e);

        CHECK(cm_cow_mount() == CM_OK, "remount after crash %s", names[pt]);
        cm_vol_stat_t after;
        cm_cow_volstat(&after);
        cm_stat_t st;
        CHECK(same("/crash/f", old), "crash %s: old content survives", names[pt]);
        CHECK(cm_vfs_stat(extra, &st) == CM_ERR_NOENT, "crash %s: half-done create gone",
              names[pt]);
        CHECK(after.super_gen == before.super_gen, "crash %s: gen %llu -> %llu", names[pt],
              (unsigned long)before.super_gen, (unsigned long)after.super_gen);
        CHECK(after.used_blocks == before.used_blocks,
              "crash %s: no leaked blocks (%llu vs %llu)", names[pt],
              (unsigned long)after.used_blocks, (unsigned long)before.used_blocks);
        get("/data/big.bin", g_rbuf, 100 * 1024 + 123, 0);
        CHECK(memcmp(g_rbuf, g_wbuf, 100 * 1024 + 123) == 0,
              "crash %s: big file untouched", names[pt]);
        cm_scrub_report_t sr;
        CHECK(cm_cow_scrub(&sr) == CM_OK, "crash %s: scrub clean", names[pt]);
        kinfo("p3: crash %s -> mounted gen %llu, old data intact, 0 leaked blocks",
              names[pt], (unsigned long)after.super_gen);
    }
    /* And the commit that is NOT interrupted is durable. */
    cm_cow_txn_begin();
    put("/crash/f", 0, "NEW-ok", 6, 0);
    CHECK(cm_cow_txn_end() == CM_OK, "clean commit");
    CHECK(cm_cow_mount() == CM_OK && same("/crash/f", "NEW-ok"), "committed data durable");
}

/* ---- 5. checksums, read-repair, scrub ---------------------------------- */

static void t_integrity(void)
{
    kinfo("--- p3: bit rot under the FS (data EIO, metadata self-heal) ---");
    fill_pattern(g_wbuf, 8192, 99);
    CHECK(put("/data/rot", CM_O_TRUNC, g_wbuf, 8192, 0) == 8192, "rot file");
    uint32_t b = cm_cow_bmap("/data/rot", 4096);
    uint8_t *raw = cm_blk_raw_sector(cm_cow_fs_lba(b) + 1);
    raw[17] ^= 0x40;                                  /* one bit, on the media */
    cm_pcache_invalidate_all();
    int64_t r = get("/data/rot", g_rbuf, 8192, 0);
    CHECK(r == CM_ERR_IO, "rotten data block must be EIO, not garbage (got %lld)", (long)r);
    CHECK(get("/data/rot", g_rbuf, 4096, 0) == 4096 && memcmp(g_rbuf, g_wbuf, 4096) == 0,
          "the healthy block of the same file still reads");
    cm_scrub_report_t sr;
    CHECK(cm_cow_scrub(&sr) == CM_ERR_IO && sr.data_errors == 1,
          "scrub finds exactly the rotten block (%llu)", (unsigned long)sr.data_errors);
    CHECK(put("/data/rot", 0, g_wbuf, 8192, 0) == 8192, "rewrite heals by COW");
    CHECK(cm_cow_scrub(&sr) == CM_OK, "scrub clean after rewrite");

    /* Metadata: rot the primary copy of inode-table block 0, remount. */
    uint32_t ia = 0, ib = 0;
    cm_cow_debug_itab0(&ia, &ib);
    cm_blk_raw_sector(cm_cow_fs_lba(ia))[300] ^= 0xFF;
    cm_vol_stat_t v;
    CHECK(cm_cow_mount() == CM_OK, "mount with a rotten itab copy");
    cm_cow_volstat(&v);
    CHECK(v.read_repairs >= 1, "read-repair healed it (%llu)", (unsigned long)v.read_repairs);
    CHECK(same("/crash/f", "NEW-ok"), "files readable after heal");
    /* Rot the DUP copy only: normal reads never notice, scrub must. */
    cm_cow_debug_itab0(&ia, &ib);
    cm_blk_raw_sector(cm_cow_fs_lba(ib))[9] ^= 0x01;
    CHECK(cm_cow_scrub(&sr) == CM_OK && sr.meta_repaired == 1,
          "scrub repaired silent DUP rot (%llu)", (unsigned long)sr.meta_repaired);
    CHECK(cm_cow_scrub(&sr) == CM_OK && sr.meta_repaired == 0, "second scrub clean");

    /* Newest superblock rots: mount falls back one generation, consistently.
     * Two clean commits first, so the fallback generation is one written
     * AFTER the rotten data block above was rewritten away. */
    put("/data/sb-pre", 0, "1", 1, 0);
    uint64_t g = cm_cow_super_gen();
    cm_blk_raw_sector(cm_cow_fs_lba((uint32_t)(g & 1)))[40] ^= 0x10;
    CHECK(cm_cow_mount() == CM_OK && cm_cow_super_gen() == g - 1,
          "bad newest superblock -> previous generation (%llu)",
          (unsigned long)cm_cow_super_gen());
    CHECK(cm_cow_scrub(&sr) == CM_OK, "previous generation is complete");
    put("/data/after-sb", 0, "ok", 2, 0);             /* rewrites the bad slot */
    CHECK(cm_cow_mount() == CM_OK && same("/data/after-sb", "ok"), "life goes on");
    kinfo("p3: integrity ok — data rot -> EIO + scrub, metadata rot -> DUP heal, "
          "torn/rotten superblock -> previous generation");
}

/* ---- 6. memory pressure: L1..L4 ---------------------------------------- */

#define P3_MAXPG 8192

static cm_anon_id_t g_pid_page[P3_MAXPG];
static uint32_t     g_pg_sum[P3_MAXPG];
static cm_id_t      g_pg_owner[P3_MAXPG];
static uint32_t     g_npg;

static void p3_noop_entry(void) { cm_syscall(CM_SYS_EXIT, 0, 0, 0, 0); }

static cm_id_t mkproc(const char *name, cm_qos_t q)
{
    cm_spawn_attr_t a = { .path = name, .entry = p3_noop_entry, .qos = q,
                          .image_pages = 1, .stack_pages = 1 };
    cm_id_t pid = 0;
    return cm_posix_spawn(&a, &pid) == CM_OK ? pid : 0;
}

static void reap(cm_id_t pid)
{
    cm_proc_t *p = cm_proc_lookup(pid);
    if (!p) return;
    if (!p->exited) {
        p->exited = 1;
        if (p->aspace_pages) { cm_commit_uncharge(p->aspace_pages); p->aspace_pages = 0; }
    }
    cm_anon_free_owner(pid);
    if (p->main && p->main->id) { p->main->state = CM_TH_DEAD; cm_thread_reap(p->main); }
    p->main = NULL;
}

static uint32_t page_sum(const uint32_t *w)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 1024; i++) { h ^= w[i]; h *= 16777619u; }
    return h;
}

/* compressible: ~1.5 KiB under WKdm. random: does not compress at all. */
static void fill_page(uint32_t *w, uint32_t seed, int random)
{
    uint32_t x = seed * 2654435761u + 0x9E3779B9u;
    for (int i = 0; i < 1024; i++) {
        if (random) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; w[i] = x; }
        else w[i] = (i & 7) == 0 ? seed ^ (uint32_t)i
                  : (i & 1) ? 0 : 0x11110000u | (uint32_t)(i & 0xF);
    }
}

/* Latency: mean, max, and p99 via the K largest samples (K = n/100 + 1). */
#define LAT_TOPK 96
typedef struct { uint64_t n, sum, max; uint64_t top[LAT_TOPK]; uint32_t ntop; } lat_t;

static void lat_add(lat_t *l, uint64_t dt)
{
    l->n++;
    l->sum += dt;
    if (dt > l->max) l->max = dt;
    /* keep top[] sorted descending, at most LAT_TOPK entries */
    uint32_t i = l->ntop < LAT_TOPK ? l->ntop++ : LAT_TOPK - 1;
    if (i == LAT_TOPK - 1 && l->ntop == LAT_TOPK && dt <= l->top[i]) return;
    while (i > 0 && l->top[i - 1] < dt) { l->top[i] = l->top[i - 1]; i--; }
    l->top[i] = dt;
}

static uint64_t lat_p99(const lat_t *l)
{
    uint64_t k = l->n / 100 + 1;
    if (k > l->ntop) k = l->ntop;
    return k ? l->top[k - 1] : 0;
}

/* Allocate n pages for pid, fill + checksum them; returns pages obtained. */
static uint32_t take(cm_id_t pid, uint32_t n, int random, lat_t *lat)
{
    uint32_t got = 0;
    for (uint32_t i = 0; i < n && g_npg < P3_MAXPG; i++) {
        uint64_t t0 = cm_rdtsc();
        cm_anon_id_t id = cm_anon_alloc(pid);
        uint32_t *w = id ? (uint32_t *)cm_anon_access(id, 1) : NULL;
        if (w) fill_page(w, g_npg * 7919u + (uint32_t)pid, random);
        uint64_t dt = cm_rdtsc() - t0;
        if (!w) { if (id) cm_anon_free(id); break; }
        if (lat) lat_add(lat, dt);
        g_pid_page[g_npg] = id;
        g_pg_sum[g_npg] = page_sum(w);
        g_pg_owner[g_npg] = pid;
        g_npg++;
        got++;
    }
    return got;
}

/* Touch every page of every LIVE owner: forces decompress / swap-in. */
static uint32_t verify_all(uint32_t *bad, uint32_t *lost)
{
    uint32_t ok = 0;
    *bad = *lost = 0;
    for (uint32_t i = 0; i < g_npg; i++) {
        if (!cm_proc_alive(g_pg_owner[i])) continue;
        const uint32_t *w = (const uint32_t *)cm_anon_access(g_pid_page[i], 0);
        /* NULL because the access itself pushed L4 into killing the owner
         * is a kill, not a lost page; only a live owner's NULL is a loss. */
        if (!w) { if (cm_proc_alive(g_pg_owner[i])) (*lost)++; continue; }
        if (page_sum(w) != g_pg_sum[i]) (*bad)++; else ok++;
    }
    return ok;
}

static uint64_t to_us(uint64_t cyc)
{
    uint64_t hz = cm_tsc_hz();
    return hz ? cyc * 1000000ull / hz : 0;
}

static void t_pressure(void)
{
    kinfo("--- p3: memory pressure (balloon RAM, overfill, verify every byte) ---");
    cm_watermarks_t wm;
    cm_mm_watermarks(&wm);
    cm_mm_stats_t s0, s;
    cm_mm_stats(&s0);
    uint64_t commit0 = s0.committed_pages;

    /* Deterministic round trip through each state before any pressure:
     * resident -> compressed (pool) -> resident, then -> swapped -> resident. */
    {
        cm_anon_id_t id = cm_anon_alloc(0);
        uint32_t *w = id ? (uint32_t *)cm_anon_access(id, 1) : NULL;
        CHECK(w != NULL, "anon alloc");
        if (w) {
            fill_page(w, 4242, 0);
            uint32_t sum = page_sum(w);
            cm_pressure_stats_t p0, p1;
            cm_pressure_stats(&p0);
            cm_anon_compress(1000000);                /* clock: 2 sweeps */
            CHECK(cm_compressed_pages() == 1, "page compressed");
            w = (uint32_t *)cm_anon_access(id, 0);
            CHECK(w && page_sum(w) == sum, "decompressed bit-exact");
            cm_anon_compress(1000000);
            cm_comp_swapout(1000000);
            CHECK(cm_swapped_pages() == 1 && cm_pool_pages() == 0, "segment swapped out");
            w = (uint32_t *)cm_anon_access(id, 0);
            CHECK(w && page_sum(w) == sum, "swapped-in bit-exact");
            cm_pressure_stats(&p1);
            CHECK(p1.decompressions == p0.decompressions + 1 && p1.swapins == p0.swapins + 1,
                  "one decompression + one swap-in");
            kinfo("p3: anon round trip resident->pool->resident->swap->resident bit-exact");
        }
        cm_anon_free(id);
    }

    /* Multiple arenas can leave only isolated order-0 pages after ballooning.
     * Prepare actor kernel stacks before pressure; payload still allocates under
     * the same pressure and all L1-L4 assertions remain in force. */
    cm_id_t reserved_idle=0,reserved_fg=0,reserved_fg2=0,reserved_a[4]={0};
    if (cm_numa_node_count()>1) {
        reserved_idle=mkproc("idle-hog",CM_QOS_IDLE);
        reserved_fg=mkproc("foreground",CM_QOS_USER_INTERACTIVE);
        reserved_fg2=mkproc("foreground-2",CM_QOS_USER_INTERACTIVE);
        static const char *names[4]={"bg-app","utility-app","default-app","initiated-app"};
        static const cm_qos_t bands[4]={CM_QOS_BACKGROUND,CM_QOS_UTILITY,CM_QOS_DEFAULT,CM_QOS_USER_INITIATED};
        for (int i=0;i<4;i++) reserved_a[i]=mkproc(names[i],bands[i]);
        CHECK(reserved_idle && reserved_fg && reserved_fg2 && reserved_a[0] && reserved_a[1] &&
              reserved_a[2] && reserved_a[3],"reserve NUMA pressure actors");
    }

    /* Balloon: pin RAM until only `target` pages are free, so the test
     * exercises real exhaustion of THIS machine without needing GBs of data. */
    uint64_t target = wm.high * 3;
    void *balloon = NULL;
    uint64_t nballoon = 0;
    while (cm_mm_free_pages() > target) {
        void *pg = cm_page_alloc(0);
        if (!pg) break;
        *(void **)pg = balloon;
        balloon = pg;
        nballoon++;
    }
    kinfo("p3: balloon holds %llu pages, %llu free (watermarks %llu/%llu/%llu)",
          (unsigned long)nballoon, (unsigned long)cm_mm_free_pages(),
          (unsigned long)wm.min, (unsigned long)wm.low, (unsigned long)wm.high);

    /* L1 source: populate the page cache with file data. */
    fill_pattern(g_wbuf, 64 * 1024, 5);
    put("/data/cached", CM_O_TRUNC, g_wbuf, 64 * 1024, 0);
    cm_pcache_invalidate_all();
    get("/data/cached", g_rbuf, 64 * 1024, 0);
    uint64_t cached = cm_file_cache_pages();

    /* Phase A: 2x the free RAM of compressible anon, four bands. */
    cm_proc_kill_log_reset();
    g_npg = 0;
    cm_id_t pa[4];
    static const cm_qos_t qa[4] = { CM_QOS_BACKGROUND, CM_QOS_UTILITY,
                                    CM_QOS_DEFAULT, CM_QOS_USER_INITIATED };
    static const char *na[4] = { "bg-app", "utility-app", "default-app", "initiated-app" };
    uint32_t want_a = (uint32_t)(target * 2);
    if (want_a > P3_MAXPG / 2) want_a = P3_MAXPG / 2;
    static lat_t la;
    memset(&la, 0, sizeof(la));
    cm_pressure_reset_max();
    uint32_t got_a = 0;
    for (int i = 0; i < 4; i++) {
        pa[i] = reserved_a[i] ? reserved_a[i] : mkproc(na[i], qa[i]);
        got_a += take(pa[i], want_a / 4, 0, &la);
    }
    cm_pressure_stats_t ps;
    cm_pressure_stats(&ps);
    cm_mm_stats(&s);
    kinfo("p3: phase A: %u/%u compressible pages into %llu free -> resident %llu, "
          "compressed %llu (pool %llu pages), swapped %llu (%llu/%llu segs)",
          got_a, want_a, (unsigned long)target, (unsigned long)s.anon_pages,
          (unsigned long)s.compressed_pages, (unsigned long)s.pool_pages,
          (unsigned long)s.swapped_pages, (unsigned long)s.swap_slots,
          (unsigned long)s.swap_total);
    kinfo("p3: phase A: L1 dropped %llu cache pages, L2 compressed %llu, "
          "L3 wrote %llu segments (%llu pages), direct reclaims %llu, kills %u",
          (unsigned long)ps.l1_discarded, (unsigned long)ps.l2_compressed,
          (unsigned long)ps.l3_segments_out, (unsigned long)ps.l3_pages_out,
          (unsigned long)ps.direct_reclaims, ps.jetsam_kills + ps.oom_kills);
    CHECK(got_a == want_a, "phase A got every page it asked for (%u/%u)", got_a, want_a);
    CHECK(cached > 0 && ps.l1_discarded > 0, "L1 discarded file cache");
    CHECK(ps.l2_compressed > 0, "L2 compressed");
    CHECK(ps.l3_segments_out > 0, "L3 swapped compressed segments");
    CHECK(ps.jetsam_kills + ps.oom_kills == 0, "no process killed while L1-L3 suffice");

    uint32_t bad, lost;
    uint32_t ok = verify_all(&bad, &lost);
    cm_pressure_stats(&ps);
    kinfo("p3: phase A verify: %u pages bit-exact, %u corrupt, %u lost "
          "(swap-ins %llu, decompressions %llu)", ok, bad, lost,
          (unsigned long)ps.swapins, (unsigned long)ps.decompressions);
    CHECK(ok == got_a && bad == 0 && lost == 0, "every phase A page intact");
    CHECK(ps.swapins > 0, "swap-in path exercised under load");

    /* L1 again: cache was dropped, the disk still has the data. */
    memset(g_rbuf, 0, 64 * 1024);
    CHECK(get("/data/cached", g_rbuf, 64 * 1024, 0) == 64 * 1024 &&
          memcmp(g_rbuf, g_wbuf, 64 * 1024) == 0, "file re-read after L1 drop");

    uint64_t hz = cm_tsc_hz();
    uint64_t p99 = lat_p99(&la);
    kinfo("p3: alloc under pressure: n=%llu avg=%llu cyc p99=%llu cyc max=%llu cyc "
          "(p99 %llu us, max %llu us%s); max reclaim work in one alloc=%llu units, "
          "compactions=%llu",
          (unsigned long)la.n, (unsigned long)(la.n ? la.sum / la.n : 0),
          (unsigned long)p99, (unsigned long)la.max, (unsigned long)to_us(p99),
          (unsigned long)to_us(la.max), hz ? "" : ", tsc hz unknown",
          (unsigned long)ps.max_alloc_work, (unsigned long)ps.l3_compactions);
    /* "Does not stall", measured two ways:
     *  - deterministic: no single allocation does more than a few reclaim
     *    clusters of work (32 pages each) — immune to host scheduling;
     *  - wall clock: p99 under one 1 ms tick. Max is reported, not
     *    asserted: in a VM it includes time the host deschedules the vCPU,
     *    which no guest kernel can control. */
    CHECK(ps.max_alloc_work <= 8 * 32, "one allocation did %llu units of reclaim work",
          (unsigned long)ps.max_alloc_work);
    if (hz)
        CHECK(to_us(p99) < 1000, "p99 alloc latency %llu us >= 1 ms",
              (unsigned long)to_us(p99));
    else
        CHECK(p99 < 5000000ull, "p99 alloc latency %llu cycles", (unsigned long)p99);

    /* Phase B: incompressible memory nobody can reclaim -> L4 jetsam. */
    cm_id_t idle = reserved_idle ? reserved_idle : mkproc("idle-hog", CM_QOS_IDLE);
    cm_id_t fg = reserved_fg ? reserved_fg : mkproc("foreground", CM_QOS_USER_INTERACTIVE);
    uint32_t got_idle = take(idle, (uint32_t)(target / 2), 1, NULL);
    /* Fits once everything else is gone (min reserve is not for users),
     * does not fit next to idle-hog: L4 must make the room. */
    uint32_t want_fg = (uint32_t)(target - 2 * wm.min);
    uint32_t got_fg = take(fg, want_fg, 1, NULL);
    cm_id_t kl[16];
    int nk = cm_proc_kill_log(kl, 16);
    cm_pressure_stats(&ps);
    kinfo("p3: phase B: idle-hog %u random pages, foreground got %u/%u random pages, "
          "%d kill(s)", got_idle, got_fg, want_fg, nk);
    for (int i = 0; i < nk; i++) {
        cm_proc_t *p = cm_proc_lookup(kl[i]);
        kinfo("p3:   kill #%d pid=%llu %s band=%s", i + 1, (unsigned long)kl[i],
              p ? p->path : "?", p && p->task ? cm_qos_name(p->task->qos) : "?");
    }
    CHECK(got_fg == want_fg, "foreground got all its memory (%u/%u)", got_fg, want_fg);
    CHECK(cm_proc_alive(fg), "foreground never killed");
    CHECK(nk >= 1 && kl[0] == idle, "first victim is the Idle band");
    int mono = 1;
    for (int i = 1; i < nk; i++) {
        cm_proc_t *a = cm_proc_lookup(kl[i - 1]), *b2 = cm_proc_lookup(kl[i]);
        if (a && b2 && b2->task->qos > a->task->qos) mono = 0;
    }
    CHECK(mono, "victims taken from most to least disposable band");
    CHECK(ps.oom_kills == 0, "jetsam sufficed, OOM killer not needed");
    ok = verify_all(&bad, &lost);
    CHECK(bad == 0 && lost == 0, "survivors' pages intact after L4 (%u ok, %u bad, %u lost)",
          ok, bad, lost);
    /* Faulting every survivor back in is pure L2/L3 work: compaction and
     * whole-segment swap-in must keep swap dense enough that it needs no
     * further kills. (It did, before both existed.) */
    CHECK(cm_proc_kill_log(kl, 16) == nk, "verifying survivors needed no extra kills (%d -> %d)",
          nk, cm_proc_kill_log(kl, 16));
    CHECK(cm_drv_kernel_alive(), "kernel allocator alive");

    /* Phase C: only foreground left -> Linux OOM killer, by badness. */
    for (int i = 0; i < 4; i++) reap(pa[i]);
    reap(idle);
    cm_id_t fg2 = reserved_fg2 ? reserved_fg2 : mkproc("foreground-2", CM_QOS_USER_INTERACTIVE);
    cm_proc_kill_log_reset();
    uint32_t got2 = take(fg2, (uint32_t)(target * 3 / 4), 1, NULL);
    nk = cm_proc_kill_log(kl, 16);
    cm_pressure_stats(&ps);
    kinfo("p3: phase C: foreground-2 got %u pages, OOM kills %u, victim pid=%llu "
          "(foreground pid=%llu holds the most)", got2, ps.oom_kills,
          (unsigned long)(nk ? kl[0] : 0), (unsigned long)fg);
    CHECK(ps.oom_kills >= 1 && nk >= 1 && kl[0] == fg,
          "OOM killer took the largest footprint when no band candidate was left");
    CHECK(cm_proc_alive(fg2), "the smaller requester survived");

    /* Teardown: everything returns. */
    reap(fg);
    reap(fg2);
    while (balloon) { void *nx = *(void **)balloon; cm_page_free(balloon, 0); balloon = nx; }
    cm_pressure_tick();
    cm_mm_stats(&s);
    kinfo("p3: after teardown free=%llu (before %llu) commit=%llu (before %llu) "
          "anon=%llu comp=%llu pool=%llu swapped=%llu segs=%llu",
          (unsigned long)s.free_pages, (unsigned long)s0.free_pages,
          (unsigned long)s.committed_pages, (unsigned long)commit0,
          (unsigned long)s.anon_pages, (unsigned long)s.compressed_pages,
          (unsigned long)s.pool_pages, (unsigned long)s.swapped_pages,
          (unsigned long)s.swap_slots);
    CHECK(s.anon_pages == 0 && s.compressed_pages == 0 && s.swapped_pages == 0 &&
          s.pool_pages == 0 && s.swap_slots == 0, "no anon/pool/swap leaked");
    CHECK(s.committed_pages <= commit0 + 8, "commit charge returned (%llu vs %llu)",
          (unsigned long)s.committed_pages, (unsigned long)commit0);
    CHECK(s.free_pages + s.file_cache_pages + 64 >= s0.free_pages + s0.file_cache_pages,
          "RAM returned (free %llu+cache %llu vs %llu+%llu)",
          (unsigned long)s.free_pages, (unsigned long)s.file_cache_pages,
          (unsigned long)s0.free_pages, (unsigned long)s0.file_cache_pages);
    CHECK(cm_pressure_tick() == CM_PRESS_IDLE, "pressure back to idle");
    kinfo("p3: pressure ok — L1 drop, L2 compress, L3 swap, swap-in bit-exact, "
          "L4 jetsam by band, OOM fallback, nothing leaked");
}

int cm_phase3_selftest(void)
{
    g_f = 0;
    t_vfs();
    t_syscalls();
    t_cow();
    t_crash();
    t_integrity();
    t_pressure();
    cm_vol_stat_t v;
    cm_cow_volstat(&v);
    kinfo("p3: vol gen=%llu used=%llu/%llu files=%llu commits=%llu cow=%llu inplace=%llu "
          "csum_err=%llu repairs=%llu AG[%u %u %u %u]",
          (unsigned long)v.super_gen, (unsigned long)v.used_blocks,
          (unsigned long)v.total_blocks, (unsigned long)v.files,
          (unsigned long)v.commits, (unsigned long)v.cow_writes,
          (unsigned long)v.inplace_writes, (unsigned long)v.csum_errors,
          (unsigned long)v.read_repairs, v.ag_used[0], v.ag_used[1], v.ag_used[2],
          v.ag_used[3]);
    kinfo("p3: phase 3 selftest %s (%d failure(s))", g_f ? "FAILED" : "PASSED", g_f);
    return g_f;
}
