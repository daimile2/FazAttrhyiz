#ifndef CAPMIX_IO_H
#define CAPMIX_IO_H

#include "types.h"
#include "config.h"
#include "ipc.h"

/* Layer 5 + phase 4:
 *   io_uring SQ/CQ          — Linux
 *   IOCP concurrency hint   — Windows
 *   blk-mq                  — Linux
 *   user-mode drivers       — DriverKit / UMDF
 *   irq top/bottom half     — classic split
 *
 * Kernel bus is tiny. A driver bug kills the driver task, not the kernel.
 */

#define CM_URING_ENTRIES 256
#define CM_MAX_IRQ       32
#define CM_SOFTIRQ_MAX   64
#define CM_MAX_DRIVERS   16
#define CM_BLK_QUEUES    4
#define CM_BLK_QD        32
#define CM_BLK_SECTOR    512

/* Ramdisk layout (8 MiB, sectors of 512 B). Three fixed partitions, so the
 * raw uring/blk selftest at LBA 0 can never land on swap or the volume.
 *   [0, 64)          raw   — uring/blk-mq selftests
 *   [64, 8256)       swap  — 4 MiB, compressed-segment swap (pressure L3)
 *   [8256, 16384)    fs    — COW volume, 1016 blocks of 4 KiB          */
#define CM_BLK_DISK_SECT   16384u
#define CM_BLK_RAW_LBA     0u
#define CM_BLK_RAW_SECT    64u
#define CM_BLK_SWAP_LBA    64u
#define CM_BLK_SWAP_SECT   8192u
#define CM_BLK_FS_LBA      8256u
#define CM_BLK_FS_SECT     (CM_BLK_DISK_SECT - CM_BLK_FS_LBA)

#define CM_OP_NOP    0
#define CM_OP_READ   1
#define CM_OP_WRITE  2
#define CM_OP_FSYNC  3
#define CM_OP_IOCTL  4

typedef struct {
    uint32_t opcode;
    uint32_t fd;
    uint64_t addr;
    uint64_t len;
    uint64_t off;
    uint64_t user_data;
} cm_sqe_t;

typedef struct {
    uint64_t user_data;
    int32_t  res;
    uint32_t flags;
} cm_cqe_t;

typedef void (*cm_irq_handler_t)(int irq, void *arg);

typedef enum {
    CM_DRV_EMPTY = 0,
    CM_DRV_ONLINE,
    CM_DRV_OFFLINE,   /* crashed or unbound — kernel still up */
} cm_drv_state_t;

#define CM_DRV_IRQ_EVENT 0x495251u
/* Sequence is the first IRQ in a coalesced packet; count is its multiplicity. */
typedef struct {
    uint32_t irq, driver_id;
    uint64_t sequence, count;
} cm_drv_irq_packet_t;

typedef struct {
    int           id;
    const char   *name;
    cm_drv_state_t state;
    int           irq;
    cm_id_t       pid;         /* user-mode driver process */
    int           chan_k;      /* WRITE-only endpoint in broker task */
    int           chan_u;
    cm_id_t       recv_object, thread_id;
    uint64_t      delivered, received, pending, pending_sequence;
    uint64_t      backpressure, dropped;
    cm_object_t *wait_endpoint; /* driver-owned reference while a recv sleeps */
    struct cm_thread *wait_thread;
    cm_error_t wait_error;
    uint64_t waits, wakes, cancels;
    char          name_storage[64];
    uint32_t      crashes;
    uint64_t      irqs;
    uint64_t      ios;
} cm_driver_t;

typedef struct {
    uint64_t lba;
    uint32_t nsect;
    uint32_t op;
    void    *buf;
    int32_t  status;
    int      qid;
    uint64_t tag;      /* caller cookie; cm_blk_rw() matches on it */
} cm_bio_t;

cm_error_t cm_irq_init(void);
cm_error_t cm_irq_request(int irq, cm_irq_handler_t h, void *arg, const char *name);
cm_error_t cm_irq_release(int irq, cm_irq_handler_t h, void *arg);
void       cm_irq_inject(int irq);          /* hosted: pretend hardware fired */
uint64_t   cm_irq_softirq_flush(void);      /* bottom half */
uint64_t   cm_irq_top_count(void);
uint64_t   cm_irq_bot_count(void);
uint64_t   cm_irq_overflow_count(void);
uint64_t   cm_irq_stale_count(void);

cm_error_t cm_uring_init(void);
cm_error_t cm_uring_submit(const cm_sqe_t *e);
cm_error_t cm_uring_complete(cm_cqe_t *out);
uint32_t   cm_uring_sq_len(void);
uint32_t   cm_uring_cq_len(void);
uint32_t   cm_uring_inflight(void);         /* IOCP-style concurrency */

cm_error_t cm_blk_init(void);
cm_error_t cm_blk_submit(const cm_bio_t *bio);
cm_error_t cm_blk_poll(cm_bio_t *out);
uint64_t   cm_blk_completed(void);
/* Synchronous helper: submit one bio through blk-mq and reap exactly its own
 * completion. Returns CM_OK or the bio's negative status. */
cm_error_t cm_blk_rw(uint32_t op, uint64_t lba, uint32_t nsect, void *buf);
int        cm_blk_online(void);
/* Test hook: raw sector access that bypasses the queue, so a selftest can
 * corrupt media the way a failing disk would (bit rot under the FS). */
uint8_t   *cm_blk_raw_sector(uint64_t lba);

cm_error_t   cm_drv_init(void);
int          cm_drv_bind(const char *name, int irq, cm_id_t pid);
int          cm_drv_bind_channel(const char *name, int irq, cm_id_t pid, int send_hid, int recv_hid);
uint64_t     cm_drv_irq_retry(void);
cm_error_t   cm_drv_receive(cm_id_t pid, int recv_hid, cm_ipc_msg_t *out);
cm_error_t cm_drv_receive_wait(cm_id_t pid,int recv_hid,cm_ipc_msg_t *out);
cm_error_t cm_drv_wait_cancel(cm_id_t pid);
int          cm_drv_has_receiver(cm_id_t pid);
void         cm_drv_process_exit(cm_id_t pid);
cm_error_t   cm_drv_crash(int id);          /* isolate: offline device, keep kernel */
cm_driver_t *cm_drv_get(int id);
int          cm_drv_kernel_alive(void);

cm_error_t cm_io_init(void);

#endif
