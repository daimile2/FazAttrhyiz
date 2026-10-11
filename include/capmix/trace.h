#ifndef CAPMIX_TRACE_H
#define CAPMIX_TRACE_H
#include "types.h"
#include "cap.h"
#include "config.h"
#define CM_TRACE_POINTS(X) \
 X(SCHED_SWITCH,"sched.switch","sched") \
 X(SCHED_ENQUEUE,"sched.enqueue","sched") \
 X(SCHED_DEQUEUE,"sched.dequeue","sched") \
 X(MM_FAULT,"mm.soft_fault","mm") \
 X(COW_SPLIT,"fs.cow_split","fs") \
 X(IPC_SEND,"ipc.send","ipc") \
 X(IPC_RECV,"ipc.recv","ipc") \
 X(UDP_TX,"udp.tx","net") \
 X(UDP_RX,"udp.rx","net") \
 X(IRQ_DELIVER,"irq.deferred_deliver","irq") \
 X(SIGN_ACCEPT,"sign.accept","security") \
 X(SIGN_REJECT,"sign.reject","security")
#define CM_TP_ENUM(id,name,category) CM_TP_##id,
typedef enum { CM_TRACE_POINTS(CM_TP_ENUM) CM_TP_COUNT } cm_tp_id_t;
#undef CM_TP_ENUM
#define CM_TP_BIT(id) (1ull<<(id))
#define CM_TP_ALL ((1ull<<CM_TP_COUNT)-1)
#ifndef CM_TP_BUILD_MASK
#define CM_TP_BUILD_MASK CM_TP_ALL
#endif
#define CM_TP_RING_SIZE 512
#define CM_RIGHT_TRACE_READ CM_RIGHT_INSPECT
#define CAP_TRACE_READ CM_RIGHT_TRACE_READ
typedef struct { unsigned id; const char *name,*category; uint64_t mask; } cm_tp_desc_t;
typedef struct {
    uint64_t tsc,cpu,task,thread,trace,parent,seq,tp,a0,a1,a2;
} cm_trace_record_t;
typedef struct {
    uint64_t cursor, lost;
    cm_trace_record_t event;
} cm_trace_read_t;
extern uint64_t cm_tp_enabled_mask;
extern const cm_tp_desc_t cm_tp_catalog[CM_TP_COUNT];
#define CM_TRACE(id,tr,a,b,c) do { \
 if ((CM_TP_BUILD_MASK & CM_TP_BIT(CM_TP_##id)) && \
     (__atomic_load_n(&cm_tp_enabled_mask,__ATOMIC_RELAXED) & CM_TP_BIT(CM_TP_##id))) \
     cm_tp_emit(CM_TP_##id,(tr),(uint64_t)(a),(uint64_t)(b),(uint64_t)(c)); \
} while (0)
#define TRACE_SCHED_SWITCH(tr,a,b,c) CM_TRACE(SCHED_SWITCH,tr,a,b,c)
#define TRACE_IPC_SEND(tr,a,b,c) CM_TRACE(IPC_SEND,tr,a,b,c)
#define TRACE_IPC_RECV(tr,a,b,c) CM_TRACE(IPC_RECV,tr,a,b,c)
void cm_tp_init(void);
void cm_trace_process_exit(cm_task_t *task);
void cm_tp_emit(unsigned tp,cm_trace_t tr,uint64_t a,uint64_t b,uint64_t c);
/* Trusted kernel policy mints the session, not an unrestricted user open. */
int cm_trace_open(cm_task_t *reader,uint64_t allowed_mask,uint64_t subject,uint64_t cpu_mask);
cm_error_t cm_trace_configure(cm_task_t *reader,int hid,uint64_t mask);
cm_error_t cm_trace_read(cm_task_t *reader,int hid,unsigned cpu,cm_trace_read_t *out);
uint64_t cm_trace_head(unsigned cpu);
/* BSP invokes this while AP schedulers are parked. Callback runs on real AP. */
int cm_smp_trace_test(void (*callback)(unsigned),unsigned rounds);
#endif
