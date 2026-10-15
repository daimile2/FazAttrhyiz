#ifndef CAPMIX_LINUX_SOCKET_H
#define CAPMIX_LINUX_SOCKET_H
#include "types.h"
struct cm_proc;
int cm_linux_socket_kind(struct cm_proc *,uint64_t);
int cm_linux_socket_is(struct cm_proc *,uint64_t);
int64_t cm_linux_socket_io(struct cm_proc *,uint64_t,uint64_t,uint64_t,int);
int64_t cm_linux_socket_dup(struct cm_proc *,uint64_t,uint64_t);
int64_t cm_linux_socket_fcntl(struct cm_proc *,uint64_t,uint64_t,uint64_t);
int cm_linux_socket_call(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,int64_t *);
unsigned cm_linux_socket_live(void);
int cm_linux_l3_selftest(void);
#endif
