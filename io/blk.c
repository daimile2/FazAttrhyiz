#include "capmix/io.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

#define CM_RAMDISK_SECT 128

static uint8_t  g_disk[CM_RAMDISK_SECT][CM_BLK_SECTOR];
static cm_bio_t g_q[CM_BLK_QUEUES][CM_BLK_QD];
static int      g_qh[CM_BLK_QUEUES], g_qt[CM_BLK_QUEUES], g_qn[CM_BLK_QUEUES];
static cm_bio_t g_done[CM_BLK_QD];
static int      g_dh, g_dt, g_dn;
static uint64_t g_done_total;
static int      g_offline; /* set when bound user driver crashes */

cm_error_t cm_blk_init(void)
{
    memset(g_disk, 0, sizeof(g_disk));
    memset(g_q, 0, sizeof(g_q));
    memset(g_done, 0, sizeof(g_done));
    memset(g_qh, 0, sizeof(g_qh));
    memset(g_qt, 0, sizeof(g_qt));
    memset(g_qn, 0, sizeof(g_qn));
    g_dh = g_dt = g_dn = 0;
    g_done_total = 0;
    g_offline = 0;
    kinfo("blk: mq %d queues x qd %d, ramdisk %d sectors",
          CM_BLK_QUEUES, CM_BLK_QD, CM_RAMDISK_SECT);
    return CM_OK;
}

void cm_blk_set_offline(int off) { g_offline = off; }

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
    if (g_offline) {
        b->status = CM_ERR_DEAD;
        complete_one(b);
        return;
    }
    if (b->lba + b->nsect > CM_RAMDISK_SECT) {
        b->status = CM_ERR_INVAL;
        complete_one(b);
        return;
    }
    for (uint32_t i = 0; i < b->nsect; i++) {
        uint8_t *sec = g_disk[b->lba + i];
        if (b->op == CM_OP_WRITE && b->buf)
            memcpy(sec, (uint8_t *)b->buf + i * CM_BLK_SECTOR, CM_BLK_SECTOR);
        else if (b->op == CM_OP_READ && b->buf)
            memcpy((uint8_t *)b->buf + i * CM_BLK_SECTOR, sec, CM_BLK_SECTOR);
    }
    b->status = (int32_t)(b->nsect * CM_BLK_SECTOR);
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
