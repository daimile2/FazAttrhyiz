#ifndef CAPMIX_LINUX_PIPE_H
#define CAPMIX_LINUX_PIPE_H
#include "types.h"
struct cm_proc;
int cm_linux_pipe_is(struct cm_proc *,uint64_t);
int64_t cm_linux_pipe_create(struct cm_proc *,uint64_t,uint64_t);
int64_t cm_linux_pipe_io(struct cm_proc *,uint64_t,uint64_t,uint64_t,int);
int64_t cm_linux_pipe_dup(struct cm_proc *,uint64_t,uint64_t);
int64_t cm_linux_pipe_fcntl(struct cm_proc *,uint64_t,uint64_t,uint64_t);
unsigned cm_linux_pipe_live(void);
#endif
