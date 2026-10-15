/* Linux v6.6 x86-64 ABI definitions, explicit widths (not host libc structs).
 * Sources: arch/x86/include/uapi/asm/stat.h (Linux-syscall-note),
 * include/linux/dirent.h, include/uapi/linux/utsname.h and uio.h at tag v6.6.
 * https://github.com/torvalds/linux/tree/v6.6
 */
#ifndef CAPMIX_LINUX_UAPI_H
#define CAPMIX_LINUX_UAPI_H
#include "types.h"
typedef struct {
    uint64_t dev,ino,nlink;
    uint32_t mode,uid,gid,pad0;
    uint64_t rdev;
    int64_t size,blksize,blocks;
    uint64_t atime,atime_nsec,mtime,mtime_nsec,ctime,ctime_nsec;
    int64_t unused[3];
} cm_linux_stat_t;
typedef struct {uint64_t ino;int64_t off;uint16_t reclen;uint8_t type;char name[];} cm_linux_dirent64_t;
/* Linux v6.6 include/uapi/linux/in.h, socket.h: AF_INET sockaddr layout.
 * Byte arrays make network endian explicit, independent of host libc. */
typedef struct {uint16_t family;uint8_t port[2],address[4],zero[8];} cm_linux_sockaddr_in_t;
_Static_assert(sizeof(cm_linux_sockaddr_in_t)==16 && offsetof(cm_linux_sockaddr_in_t,port)==2 && offsetof(cm_linux_sockaddr_in_t,address)==4,"Linux sockaddr_in layout");
typedef uint32_t cm_linux_socklen_t;
_Static_assert(sizeof(cm_linux_socklen_t)==4,"Linux socklen_t");
typedef struct {uint64_t base,len;} cm_linux_iovec_t;
typedef struct {char sysname[65],nodename[65],release[65],version[65],machine[65],domainname[65];} cm_linux_utsname_t;
_Static_assert(sizeof(cm_linux_stat_t)==144,"Linux v6.6 x86-64 stat size");
#define CM_UAPI_OFFSET(t,m,n) _Static_assert(offsetof(t,m)==n,"Linux offset " #m)
CM_UAPI_OFFSET(cm_linux_stat_t,mode,24);CM_UAPI_OFFSET(cm_linux_stat_t,uid,28);
CM_UAPI_OFFSET(cm_linux_stat_t,rdev,40);CM_UAPI_OFFSET(cm_linux_stat_t,size,48);
CM_UAPI_OFFSET(cm_linux_stat_t,blksize,56);CM_UAPI_OFFSET(cm_linux_stat_t,blocks,64);
CM_UAPI_OFFSET(cm_linux_stat_t,atime,72);CM_UAPI_OFFSET(cm_linux_stat_t,mtime,88);
CM_UAPI_OFFSET(cm_linux_stat_t,ctime,104);CM_UAPI_OFFSET(cm_linux_stat_t,unused,120);
CM_UAPI_OFFSET(cm_linux_dirent64_t,off,8);CM_UAPI_OFFSET(cm_linux_dirent64_t,reclen,16);
CM_UAPI_OFFSET(cm_linux_dirent64_t,type,18);CM_UAPI_OFFSET(cm_linux_dirent64_t,name,19);
_Static_assert(sizeof(cm_linux_iovec_t)==16 && sizeof(cm_linux_utsname_t)==390,"Linux iovec/utsname size");
#undef CM_UAPI_OFFSET
#endif
