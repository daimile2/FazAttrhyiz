#ifndef CAPMIX_OBS_H
#define CAPMIX_OBS_H

#include "types.h"

/* Layer 7 / phase 8: eBPF-style compute + ETW identity.
 * Observation is a capability (conflict #7). Static tracepoints preferred.
 */

typedef enum {
    CM_EV_BOOT = 1,
    CM_EV_ALLOC,
    CM_EV_FREE,
    CM_EV_IPC_SEND,
    CM_EV_IPC_RECV,
    CM_EV_IPC_HANDOFF,
    CM_EV_SCHED_PICK,
    CM_EV_SCHED_SWITCH,
    CM_EV_QOS_BOOST,
    CM_EV_TURNSTILE,
    CM_EV_PRESSURE,
    CM_EV_OOM,
    CM_EV_CAP_MINT,
    CM_EV_CAP_DROP,
    CM_EV_SYS,
    CM_EV_NET,
    CM_EV_FS,
} cm_event_kind_t;

typedef struct {
    cm_tick_t       ts;
    cm_trace_t      trace;
    cm_event_kind_t kind;
    uint32_t        cpu;
    uint32_t        tid;
    uint64_t        a0, a1, a2;
} cm_event_t;

#define CM_OBS_PROVIDERS 8
#define CM_OBS_SESSIONS  4
#define CM_HIST_BUCKETS  16

typedef struct {
    uint32_t id;
    const char *name;
    uint32_t event_mask;
    int      enabled;
} cm_obs_provider_t;

typedef struct {
    uint32_t id;
    uint32_t provider_mask;
    cm_id_t  cap_owner;     /* who may read this session */
    uint64_t seen;
    int      live;
} cm_obs_session_t;

typedef struct {
    uint64_t bounds[CM_HIST_BUCKETS];
    uint64_t count[CM_HIST_BUCKETS];
    uint64_t total;
    uint64_t sum;
} cm_hist_t;

void       cm_obs_init(void);
void       cm_obs_enable(int on);
int        cm_obs_enabled(void);
cm_trace_t cm_trace_new(void);
cm_trace_t cm_trace_child(cm_trace_t parent);
void       cm_obs_emit(cm_trace_t tr, cm_event_kind_t kind,
                       uint64_t a0, uint64_t a1, uint64_t a2);
void       cm_obs_dump(int max_events);
uint64_t   cm_obs_count(void);

int        cm_obs_register_provider(const char *name, uint32_t mask);
int        cm_obs_open_session(uint32_t provider_mask, cm_id_t owner);
cm_error_t cm_obs_authorize(cm_id_t owner, int session);
uint64_t   cm_obs_session_seen(int session);

void       cm_hist_init(cm_hist_t *h, uint64_t scale);
void       cm_hist_add(cm_hist_t *h, uint64_t v);
void       cm_hist_dump(const char *name, const cm_hist_t *h);

#define CM_TRACEPOINT(kind, a0, a1, a2) \
    cm_obs_emit(cm_trace_new(), (kind), (uint64_t)(a0), (uint64_t)(a1), (uint64_t)(a2))

#endif
