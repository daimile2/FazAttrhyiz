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
    int        handles[CM_IPC_MAX_HANDLES];
} cm_ipc_msg_t;

typedef struct cm_channel cm_channel_t;

cm_error_t cm_channel_create(cm_task_t *owner, int *out_a, int *out_b);
cm_error_t cm_ipc_send(cm_task_t *sender, int hid, const cm_ipc_msg_t *msg);
cm_error_t cm_ipc_recv(cm_task_t *receiver, int hid, cm_ipc_msg_t *out);
cm_error_t cm_ipc_call(cm_task_t *sender, int hid,
                       const cm_ipc_msg_t *req, cm_ipc_msg_t *reply);

/* Phase-1 acceptance: measure round-trip. Skeleton dies if this is slow. */
uint64_t   cm_ipc_bench_rtt(int rounds);
uint64_t   cm_ipc_bench_rtt_switched(int rounds);
void       cm_ipc_dump(void);

#endif
