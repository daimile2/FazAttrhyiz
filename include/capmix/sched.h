#ifndef CAPMIX_SCHED_H
#define CAPMIX_SCHED_H

#include "types.h"
#include "config.h"
#include "cap.h"

/* Layer 2 mix:
 *   EEVDF                  — Linux 6.6+
 *   QoS bands              — macOS
 *   turnstile + QoS propagate along IPC wait chain — macOS (required for caps)
 *   thread as sched unit   — NT / Mach
 *   coalition / job        — macOS
 *   aging + reserved bw    — conflict #8, else BACKGROUND starves
 */

typedef enum {
    CM_QOS_USER_INTERACTIVE = 0,
    CM_QOS_USER_INITIATED,
    CM_QOS_DEFAULT,
    CM_QOS_UTILITY,
    CM_QOS_BACKGROUND,
    CM_QOS_IDLE,
    CM_QOS_NBANDS
} cm_qos_t;

typedef enum {
    CM_TH_UNUSED = 0,
    CM_TH_RUNNABLE,
    CM_TH_RUNNING,
    CM_TH_BLOCKED,
    CM_TH_SLEEPING,
    CM_TH_DEAD,
} cm_th_state_t;

typedef void (*cm_th_entry_t)(void *arg);

typedef struct cm_thread cm_thread_t;

struct cm_task {
    cm_id_t      id;
    const char  *name;
    cm_handle_t  handles[CM_HANDLES_PER_TASK];
    int          handle_cap;
    cm_qos_t     qos;
    int          oom_score_adj;
    unsigned     memory_node;     /* NUMA binding; stage 0 supports node0 */
    uint32_t     jetsam_band;     /* L4 kill order */
    cm_object_t *self_obj;
};

struct cm_thread {
    cm_id_t       id;
    cm_task_t    *task;
    cm_th_state_t state;
    cm_qos_t      qos;
    cm_th_entry_t entry;
    void         *arg;

    /* EEVDF */
    int64_t       weight;         /* scaled, default 1024 */
    int64_t       vruntime;
    int64_t       ve;             /* eligible virtual time */
    int64_t       vdeadline;
    int64_t       lag;
    int64_t       slice;

    /* turnstile / priority inheritance along IPC */
    cm_thread_t  *blocked_on;
    int           pi_boost;

    uint64_t      run_ticks;
    int           rq_index;       /* -1 if not queued */
    int           ctx_slot;       /* index into g_ctx[] */
    void         *kstack;
};

void         cm_sched_init(void);
cm_task_t   *cm_task_create(const char *name, cm_qos_t qos);
cm_thread_t *cm_thread_create(cm_task_t *task, cm_th_entry_t entry, void *arg, cm_qos_t qos);
void         cm_thread_block(cm_thread_t *th, cm_thread_t *on);
void         cm_thread_wake(cm_thread_t *th);
void         cm_thread_reap(cm_thread_t *th);
cm_thread_t *cm_eevdf_pick(void);
void         cm_sched_tick(void);
void         cm_sched_yield(void);
int          cm_sched_run_until_idle(int max_steps);
cm_thread_t *cm_sched_current(void);
void         cm_sched_set_current(cm_thread_t *th);
void         cm_sched_set_current(cm_thread_t *th);
void         cm_turnstile_propagate(cm_thread_t *waiter);
void         cm_sched_dump(void);

const char  *cm_qos_name(cm_qos_t q);

#endif
