#ifndef CAPMIX_IPC_H
#define CAPMIX_IPC_H

#include "types.h"
#include "config.h"
#include "cap.h"
#include "obs.h"

/* Layer 3 — skeleton core.
 *   capability + channel     — Mach / Zircon / seL4
 *   sync IPC + direct switch — L4 family (not Mach 3.0 50–80% tax)
 *   small msg in "registers" — L4
 *   large zero-copy VMO      — Mach memory-mapping
 *   server direct-read       — Windows ALPC mode 3
 *   POSIX fd on top          — Fuchsia fdio (compat only)
 *
 * Every message carries a trace_id from birth. Cannot be retrofitted.
 */

#define CM_IPC_MAX_HANDLES 4

typedef struct {
    cm_trace_t trace;
    uint32_t   opcode;
    uint16_t   len;
    uint16_t   nhandles;
    uint8_t    data[CM_IPC_SMALL_BYTES];
    /* send: sender-local IDs moved on success; recv: receiver-local IDs.
     * Failed send leaves handles intact; failed recv leaves the queue intact.
     * Duplicate before send to keep a local handle. Rights never increase. */
    int        handles[CM_IPC_MAX_HANDLES];
} cm_ipc_msg_t;

typedef struct cm_channel cm_channel_t;

cm_error_t cm_channel_create(cm_task_t *owner, int *out_a, int *out_b);
cm_error_t cm_ipc_send(cm_task_t *sender, int hid, const cm_ipc_msg_t *msg);
cm_error_t cm_ipc_recv(cm_task_t *receiver, int hid, cm_ipc_msg_t *out);
/* Bottom-half safe: queue and wake, never directly switch into the peer. */
cm_error_t cm_ipc_send_deferred(cm_task_t *sender, int hid, const cm_ipc_msg_t *msg);
/* Poll receive: empty -> AGAIN without parking the caller. */
cm_error_t cm_ipc_try_recv(cm_task_t *receiver, int hid, cm_ipc_msg_t *out);
cm_error_t cm_ipc_try_recv_object(cm_task_t *receiver, int hid, cm_id_t object, cm_ipc_msg_t *out);
/* Export a WRITE-only sender for a validated opposite-endpoint pair. */
cm_error_t cm_ipc_export_sender(cm_task_t *owner, int send_hid, int recv_hid,
                                cm_task_t *broker, int *out_hid, cm_id_t *recv_object);
struct cm_thread;
/* Prepare under caller lifecycle lock; never switches. Stable ref only if parked. */
cm_error_t cm_ipc_wait_prepare(cm_task_t *task,int hid,cm_id_t expected,
                              struct cm_thread *thread,cm_object_t **held,bool *parked);
void cm_ipc_wait_cancel(cm_object_t *held,struct cm_thread *thread,cm_id_t thread_id,bool wake);
cm_error_t cm_ipc_call(cm_task_t *sender, int hid,
                       const cm_ipc_msg_t *req, cm_ipc_msg_t *reply);

/* Phase-1 acceptance: measure round-trip. Skeleton dies if this is slow. */
uint64_t   cm_ipc_bench_rtt(int rounds);
uint64_t   cm_ipc_bench_rtt_switched(int rounds);
void       cm_ipc_dump(void);

#endif
