#ifndef CM_USER_RUNTIME_H
#define CM_USER_RUNTIME_H
#include "capmix/control.h"
#include "capmix/fs.h"
#include "capmix/syscall.h"
#include <stddef.h>
long ucall(unsigned nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d);
extern int uconsole,ucontrol,uboot;
void uputs(const char *s);
void unum(int64_t number);
void uyield(void);
int ueq(const char *,const char *);
unsigned ulen(const char *);
int ucpl(void);
long uspawn(const char *,const char *const *,const char *const *,unsigned,cm_user_child_t *);
long uwait(int,cm_user_wait_t *,unsigned);
#endif
