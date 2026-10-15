#ifndef CAPMIX_TCP_H
#define CAPMIX_TCP_H
#include "net.h"
#define CM_TCP_CONNECTIONS 8u
#define CM_TCP_BACKLOG_MAX 4u
#define CM_TCP_ACCEPT_TTL_MS 10000u
#define CM_TCP_MSS 536u
#define CM_TCP_RX_BYTES 4096u
#define CM_TCP_RTO_MS 200u
#define CM_TCP_RTO_MAX_MS 1600u
#define CM_TCP_RETRIES 4u /* initial transmission + four retransmissions */
cm_error_t cm_tcp_init(void);
cm_error_t cm_tcp_open(cm_task_t *,int *);
cm_error_t cm_tcp_connect(cm_task_t *,int,int,uint32_t,uint16_t,unsigned);
/* Local binding + metadata only; transport/RTO machinery is unchanged. */
typedef struct {int ifid;uint32_t local,remote;uint16_t sport,dport;} cm_tcp_endpoints_t;
cm_error_t cm_tcp_bind(cm_task_t *,int,int,uint16_t);
cm_error_t cm_tcp_endpoints(cm_task_t *,int,cm_tcp_endpoints_t *);
cm_error_t cm_tcp_listen(cm_task_t *,int,int,uint16_t,unsigned);
/* Nonblocking: returns a new capability or AGAIN, never a borrowed queue slot. */
int64_t cm_tcp_accept(cm_task_t *,int);
int64_t cm_tcp_send(cm_task_t *,int,const void *,uint32_t,unsigned);
int64_t cm_tcp_recv(cm_task_t *,int,void *,uint32_t);
/* Consumes this handle after graceful close or failure; aliases share state. */
cm_error_t cm_tcp_close(cm_task_t *,int,unsigned);
void cm_tcp_process_exit(cm_task_t *);
cm_error_t cm_tcp_input(int,const uint8_t *,uint16_t);
int cm_tcp_mechanism_selftest(void);
unsigned cm_tcp_live(void);
void cm_tcp_poll(void);
/* Trusted selftest only, no user/global packet injection ABI. */
cm_error_t cm_tcp_test_drop(cm_task_t *,int,unsigned);
uint64_t cm_tcp_retransmits(void);
#endif
