#include "capmix/io.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>
#ifdef CAPMIX_HOSTED
#include <stdlib.h>
#endif

/* 8 MiB ramdisk. Bare metal takes it from the buddy (one order-11 block)
 * so it lives in HHDM-mapped RAM like every other kernel allocation; hosted
 * mallocs it so it does not eat a third of the 16 MiB simulated arena the
 * pressure selftest is trying to fill. */
static uint8_t *g_disk;
static cm_bio_t g_q[CM_BLK_QUEUES][CM_BLK_QD];
static int      g_qh[CM_BLK_QUEUES], g_qt[CM_BLK_QUEUES], g_qn[CM_BLK_QUEUES];
static cm_bio_t g_done[CM_BLK_QD];
static int      g_dh, g_dt, g_dn;
static uint64_t g_done_total;
static uint64_t g_tag_next = 1;
static int      g_offline; /* set when bound user driver crashes */

cm_error_t cm_blk_init(void)
{
    if (!g_disk) {
#ifdef CAPMIX_HOSTED
        g_disk = (uint8_t *)malloc((size_t)CM_BLK_DISK_SECT * CM_BLK_SECTOR);
#else
        g_disk = (uint8_t *)cm_page_alloc(11);   /* 2048 pages = 8 MiB */
#endif
    }
    if (!g_disk) {
        kerr("blk: cannot allocate %u-sector ramdisk", CM_BLK_DISK_SECT);
        return CM_ERR_NOMEM;
    }
    memset(g_disk, 0, (size_t)CM_BLK_DISK_SECT * CM_BLK_SECTOR);
    memset(g_q, 0, sizeof(g_q));
    memset(g_done, 0, sizeof(g_done));
    memset(g_qh, 0, sizeof(g_qh));
    memset(g_qt, 0, sizeof(g_qt));
    memset(g_qn, 0, sizeof(g_qn));
    g_dh = g_dt = g_dn = 0;
    g_done_total = 0;
    g_offline = 0;
    kinfo("blk: mq %d queues x qd %d, ramdisk %u sectors "
          "(raw %u / swap %u / fs %u)",
          CM_BLK_QUEUES, CM_BLK_QD, CM_BLK_DISK_SECT,
          CM_BLK_RAW_SECT, CM_BLK_SWAP_SECT, CM_BLK_FS_SECT);
    return CM_OK;
}

void cm_blk_set_offline(int off) { g_offline = off; }
int  cm_blk_online(void)         { return g_disk && !g_offline; }

uint8_t *cm_blk_raw_sector(uint64_t lba)
{
    if (!g_disk || lba >= CM_BLK_DISK_SECT) return NULL;
    return g_disk + lba * CM_BLK_SECTOR;
}

static void complete_one(cm_bio_t *b)
{
    if (g_dn >= CM_BLK_QD) return;
    g_done[g_dt] = *b;
    g_dt = (g_dt + 1) % CM_BLK_QD;
    g_dn++;
    g_done_total++;
}

static void service(cm_bio_t *b)
{
    if (g_offline || !g_disk) {
        b->status = CM_ERR_DEAD;
        complete_one(b);
        return;
    }
    if (b->lba + b->nsect > CM_BLK_DISK_SECT) {
        b->status = CM_ERR_INVAL;
        complete_one(b);
        return;
    }
    uint8_t *sec = g_disk + b->lba * CM_BLK_SECTOR;
    size_t   n   = (size_t)b->nsect * CM_BLK_SECTOR;
    if (b->op == CM_OP_WRITE && b->buf)
        memcpy(sec, b->buf, n);
    else if (b->op == CM_OP_READ && b->buf)
        memcpy(b->buf, sec, n);
    b->status = (int32_t)n;
    complete_one(b);
}

cm_error_t cm_blk_submit(const cm_bio_t *bio)
{
    if (!bio) return CM_ERR_INVAL;
    int q = bio->qid % CM_BLK_QUEUES;
    if (q < 0) q = 0;
    if (g_qn[q] >= CM_BLK_QD) return CM_ERR_AGAIN;
    g_q[q][g_qt[q]] = *bio;
    g_qt[q] = (g_qt[q] + 1) % CM_BLK_QD;
    g_qn[q]++;
    /* Hosted: service inline after queueing so blk-mq shape stays,
     * completion still goes through the CQ. */
    cm_bio_t *b = &g_q[q][g_qh[q]];
    g_qh[q] = (g_qh[q] + 1) % CM_BLK_QD;
    g_qn[q]--;
    service(b);
    return CM_OK;
}

cm_error_t cm_blk_poll(cm_bio_t *out)
{
    if (!out || !g_dn) return CM_ERR_AGAIN;
    *out = g_done[g_dh];
    g_dh = (g_dh + 1) % CM_BLK_QD;
    g_dn--;
    return CM_OK;
}

uint64_t cm_blk_completed(void) { return g_done_total; }

/* Reap the completion carrying `tag` and leave every other entry in order.
 * cm_blk_poll() hands back the OLDEST completion, which may belong to the
 * uring path; a synchronous caller that took whatever came first would
 * steal someone else's result. */
static int reap_tag(uint64_t tag, cm_bio_t *out)
{
    for (int k = 0; k < g_dn; k++) {
        int i = (g_dh + k) % CM_BLK_QD;
        if (g_done[i].tag != tag) continue;
        *out = g_done[i];
        for (int m = k; m + 1 < g_dn; m++)
            g_done[(g_dh + m) % CM_BLK_QD] = g_done[(g_dh + m + 1) % CM_BLK_QD];
        g_dt = (g_dt + CM_BLK_QD - 1) % CM_BLK_QD;
        g_dn--;
        return 1;
    }
    return 0;
}

cm_error_t cm_blk_rw(uint32_t op, uint64_t lba, uint32_t nsect, void *buf)
{
    if (!buf || nsect == 0) return CM_ERR_INVAL;
    cm_bio_t bio;
    memset(&bio, 0, sizeof(bio));
    bio.op = op;
    bio.lba = lba;
    bio.nsect = nsect;
    bio.buf = buf;
    bio.qid = (int)(lba & (CM_BLK_QUEUES - 1));
    bio.tag = (1ull << 63) | g_tag_next++;
    cm_error_t e = cm_blk_submit(&bio);
    if (e != CM_OK) return e;
    cm_bio_t done;
    if (!reap_tag(bio.tag, &done)) return CM_ERR_AGAIN;
    return done.status < 0 ? (cm_error_t)done.status : CM_OK;
}
