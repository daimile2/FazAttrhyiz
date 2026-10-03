#ifndef CAPMIX_IO_H
#define CAPMIX_IO_H

#include "types.h"
#include "config.h"

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

typedef struct {
    int           id;
    const char   *name;
    cm_drv_state_t state;
    int           irq;
    cm_id_t       pid;         /* user-mode driver process */
    int           chan_k;      /* kernel endpoint handle in kernel task */
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
} cm_bio_t;

cm_error_t cm_irq_init(void);
cm_error_t cm_irq_request(int irq, cm_irq_handler_t h, void *arg, const char *name);
void       cm_irq_inject(int irq);          /* hosted: pretend hardware fired */
uint64_t   cm_irq_softirq_flush(void);      /* bottom half */
uint64_t   cm_irq_top_count(void);
uint64_t   cm_irq_bot_count(void);

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

cm_error_t   cm_drv_init(void);
int          cm_drv_bind(const char *name, int irq, cm_id_t pid);
cm_error_t   cm_drv_crash(int id);          /* isolate: offline device, keep kernel */
cm_driver_t *cm_drv_get(int id);
int          cm_drv_kernel_alive(void);

cm_error_t cm_io_init(void);

#endif
