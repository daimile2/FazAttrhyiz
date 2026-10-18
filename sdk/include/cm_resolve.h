#ifndef CM_RESOLVE_H
#define CM_RESOLVE_H
#include "capmix.h"
#include "cm_hosts.h"
static inline long cm_resolve_read(void *ctx,void *p,size_t n){return cm_read(*(long *)ctx,p,n);}
static inline int cm_resolve_ipv4(const char *name,uint32_t *out)
{
    long fd=cm_open("/etc/hosts");
    /* Native NOENT=-12. Only absence falls back; permission/I/O errors stay visible. */
    if(fd<0)return fd==-12?cm_hosts_resolve(name,out,0,0):CM_HOSTS_IO;
    int e=cm_hosts_resolve(name,out,cm_resolve_read,&fd);
    long closed=cm_close(fd);return !e && closed<0?CM_HOSTS_IO:e;
}
#endif
