#include "capmix/io.h"
#include "capmix/klog.h"

#include <string.h>

static cm_sqe_t g_sq[CM_URING_ENTRIES];
static cm_cqe_t g_cq[CM_URING_ENTRIES];
static uint32_t g_sqh, g_sqt, g_sqn;
static uint32_t g_cqh, g_cqt, g_cqn;
static uint32_t g_inflight;

cm_error_t cm_uring_init(void)
{
    memset(g_sq, 0, sizeof(g_sq));
    memset(g_cq, 0, sizeof(g_cq));
    g_sqh = g_sqt = g_sqn = 0;
    g_cqh = g_cqt = g_cqn = 0;
    g_inflight = 0;
    kinfo("io: uring SQ/CQ %d entries, IOCP inflight counter on", CM_URING_ENTRIES);
    return CM_OK;
}

uint32_t cm_uring_sq_len(void) { return g_sqn; }
uint32_t cm_uring_cq_len(void) { return g_cqn; }
uint32_t cm_uring_inflight(void) { return g_inflight; }

static void cq_push(uint64_t ud, int32_t res)
{
    if (g_cqn >= CM_URING_ENTRIES) return;
    g_cq[g_cqt].user_data = ud;
    g_cq[g_cqt].res = res;
    g_cq[g_cqt].flags = 0;
    g_cqt = (g_cqt + 1) % CM_URING_ENTRIES;
    g_cqn++;
    if (g_inflight) g_inflight--;
}

cm_error_t cm_uring_submit(const cm_sqe_t *e)
{
    if (!e) return CM_ERR_INVAL;
    if (g_sqn >= CM_URING_ENTRIES) return CM_ERR_AGAIN;
    g_sq[g_sqt] = *e;
    g_sqt = (g_sqt + 1) % CM_URING_ENTRIES;
    g_sqn++;
    g_inflight++;

    cm_sqe_t sq = g_sq[g_sqh];
    g_sqh = (g_sqh + 1) % CM_URING_ENTRIES;
    g_sqn--;

    if (sq.opcode == CM_OP_NOP) {
        cq_push(sq.user_data, 0);
        return CM_OK;
    }
    if (sq.opcode == CM_OP_READ || sq.opcode == CM_OP_WRITE) {
        cm_bio_t bio;
        memset(&bio, 0, sizeof(bio));
        bio.op = sq.opcode;
        bio.lba = sq.off / CM_BLK_SECTOR;
        bio.nsect = (uint32_t)((sq.len + CM_BLK_SECTOR - 1) / CM_BLK_SECTOR);
        if (bio.nsect == 0) bio.nsect = 1;
        bio.buf = (void *)(uintptr_t)sq.addr;
        bio.qid = (int)(sq.user_data & 3);
        cm_error_t err = cm_blk_submit(&bio);
        if (err != CM_OK) {
            cq_push(sq.user_data, err);
            return err;
        }
        cm_bio_t done;
        if (cm_blk_poll(&done) == CM_OK)
            cq_push(sq.user_data, done.status);
        else
            cq_push(sq.user_data, CM_ERR_AGAIN);
        return CM_OK;
    }
    cq_push(sq.user_data, CM_ERR_NOTSUP);
    return CM_ERR_NOTSUP;
}

cm_error_t cm_uring_complete(cm_cqe_t *out)
{
    if (!out || !g_cqn) return CM_ERR_AGAIN;
    *out = g_cq[g_cqh];
    g_cqh = (g_cqh + 1) % CM_URING_ENTRIES;
    g_cqn--;
    return CM_OK;
}
