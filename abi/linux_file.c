#include "capmix/linux_pipe.h"
#include "capmix/abi.h"
#include "capmix/linux_socket.h"
#include "capmix/linux_uapi.h"
#include "capmix/proc.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include "capmix/mm.h"
#include "capmix/config.h"
#include <string.h>
#if CM_LINUX_ABI
#define AT_FDCWD (-100)
static cm_proc_t *caller(void){return cm_proc_current();}
static int valid(uint64_t fd){return fd<CM_FD_MAX && caller()->fd[fd]>=0;}
static int acquire(uint64_t fd,cm_rights_t need,cm_object_t **o)
{
    if(!valid(fd))return -9;
    int e=cm_native_file_acquire((int)fd,need,o);
    return e==CM_ERR_DENIED?-9:e? -9:0;
}
/* L1 has a fixed cwd=/; resolve .. lexically because this FS has no symlinks. */
static int path_at(int dirfd,uint64_t address,char out[128])
{
    char input[128],base[128]="/";unsigned len=0;
    for(;len<sizeof(input);len++) {
        if(cm_copyin(caller()->elf,input+len,address+len,1))return -14;
        if(!input[len])break;
    }
    if(len==sizeof(input))return -36;
    if(!len)return -2;
    if(input[0]!='/' && dirfd!=AT_FDCWD) {
        cm_object_t *o=NULL;int e=acquire((uint64_t)dirfd,CM_RIGHT_READ,&o);if(e)return e;
        cm_stat_t st;int result=cm_vfs_fstat(o->payload,&st);
        if(!result && st.kind!=CM_INO_DIR)result=CM_ERR_NOTDIR;
        if(!result)result=cm_vfs_file_path(o->payload,base,sizeof(base));
        cm_obj_release(o);if(result)return (int)cm_linux_errno(result);
    }
    char joined[256];size_t n=0;
    if(input[0]!='/'){size_t b=strlen(base);memcpy(joined,base,b);n=b;if(n && joined[n-1]!='/')joined[n++]='/';}
    memcpy(joined+n,input,len+1);n+=len;
    unsigned at=0,used=1;out[0]='/';
    while(at<n) {
        while(joined[at]=='/')at++;
        unsigned begin=at;while(joined[at] && joined[at]!='/')at++;
        unsigned size=at-begin;if(!size)break;
        if(size==1 && joined[begin]=='.')continue;
        if(size==2 && joined[begin]=='.' && joined[begin+1]=='.') {
            if(used>1){used--;while(used>1 && out[used-1]!='/')used--;}
            continue;
        }
        if(used>1 && out[used-1]!='/')out[used++]='/';
        if(used+size>=128)return -36;
        memcpy(out+used,joined+begin,size);used+=size;
    }
    if(used>1 && out[used-1]=='/')used--;
    out[used]=0;return 0;
}
static int64_t open_at(int dirfd,uint64_t name,uint64_t flags)
{
    const uint64_t allowed=3|0x40|0x80|0x200|0x400|0x8000|0x10000|0x20000|0x80000;
    if((flags&~allowed) || (flags&3)==3)return -22;
    char path[128];int e=path_at(dirfd,name,path);if(e)return e;
    int room=0;for(unsigned i=0;i<CM_FD_MAX;i++)room|=caller()->fd[i]<0;if(!room)return -24;
    cm_vfs_lock();cm_stat_t st;int exists=cm_vfs_stat(path,&st);
    if(!exists && (flags&0xc0)==0xc0){cm_vfs_unlock();return -17;}
    if(flags&0x10000) {
        if(exists || st.kind!=CM_INO_DIR){cm_vfs_unlock();return exists?cm_linux_errno(exists):-20;}
    }
    int64_t fd=cm_native_open_path(path,(uint32_t)(flags&(3|0x40|0x200|0x400)));
    cm_vfs_unlock();if(fd<0)return cm_linux_errno(fd);
    caller()->linux_console[fd]=0;caller()->linux_cloexec[fd]=!!(flags&0x80000);
    if(fd<3)caller()->stdio_mask|=1u<<fd;
    return fd;
}
/* Called under the group fd gate. Only FILE and synthetic console slots
 * may change; blocking pipe/socket lifetimes remain outside this rollout. */
static int grouped_file_slot(uint64_t fd)
{
    if(caller()->linux_console[fd])return 1;
    cm_object_t *o=NULL;
    if(cm_native_file_acquire((int)fd,0,&o))return 0;
    cm_obj_release(o);return 1;
}
static int64_t close_file(uint64_t fd)
{
    if(!valid(fd))return -9;
    if(cm_linux_group(caller())->linux_grouped && (fd<3 || !grouped_file_slot(fd)))return -95;
    if(caller()->linux_console[fd]){caller()->fd[fd]=-1;caller()->linux_console[fd]=0;caller()->linux_cloexec[fd]=0;return 0;}
    int64_t r=cm_native_close(fd);if(!r)caller()->linux_cloexec[fd]=0;
    return cm_linux_errno(r);
}
static int64_t read_file(uint64_t fd,uint64_t address,uint64_t n)
{
    if(!valid(fd))return -9;
    if(cm_linux_pipe_is(caller(),fd))return cm_linux_pipe_io(caller(),fd,address,n,0);
    if(cm_linux_socket_is(caller(),fd))return cm_linux_socket_io(caller(),fd,address,n,0);
    if(!cm_user_access(caller()->elf,address,n,1))return -14;
    if(caller()->linux_console[fd])return caller()->linux_console[fd]==1?0:-9;
    cm_object_t *o=NULL;int e=acquire(fd,CM_RIGHT_READ,&o);if(e)return e;
    cm_stat_t st;e=cm_vfs_fstat(o->payload,&st);cm_obj_release(o);
    if(e)return cm_linux_errno(e);
    if(st.kind==CM_INO_DIR)return -21;
    return cm_linux_errno(cm_native_read(fd,address,n));
}
static int64_t dup_to(uint64_t fd,uint64_t target)
{
    if(!valid(fd) || target>=CM_FD_MAX)return -9;
    if(cm_linux_group(caller())->linux_grouped &&
       (target<3 || !grouped_file_slot(fd) || (valid(target) && !grouped_file_slot(target))))return -95;
    if(fd==target)return (int64_t)target;
    if(cm_linux_pipe_is(caller(),fd))return cm_linux_pipe_dup(caller(),fd,target);
    if(cm_linux_socket_is(caller(),fd))return cm_linux_socket_dup(caller(),fd,target);
    if(caller()->linux_console[fd]) {
        if(valid(target))(void)close_file(target);
        caller()->fd[target]=0;caller()->linux_console[target]=caller()->linux_console[fd];
    } else {
        int64_t r=cm_native_dup2((int)fd,(int)target);if(r<0)return cm_linux_errno(r);
    }
    caller()->linux_cloexec[target]=0;return (int64_t)target;
}
static void convert_stat(cm_linux_stat_t *out,const cm_stat_t *st)
{
    memset(out,0,sizeof(*out));out->dev=1;out->ino=st->ino;out->nlink=st->nlink;
    out->mode=st->kind==CM_INO_DIR?0040755:0100644;out->size=(int64_t)st->size;
    out->blksize=4096;out->blocks=(int64_t)(st->blocks*8);
}
static int64_t stat_call(uint64_t fd,int dirfd,uint64_t name,uint64_t address,int byfd)
{
    if(!cm_user_access(caller()->elf,address,sizeof(cm_linux_stat_t),1))return -14;
    cm_linux_stat_t out;cm_stat_t st;int e;
    if(byfd) {
        if(!valid(fd))return -9;
        if(cm_linux_pipe_is(caller(),fd)){memset(&out,0,sizeof(out));out.mode=0010600;out.nlink=1;out.blksize=512;return cm_copyout(caller()->elf,address,&out,sizeof(out))?-14:0;}
        if(cm_linux_socket_is(caller(),fd)) {
            memset(&out,0,sizeof(out));out.mode=0140666;out.nlink=1;out.blksize=4096;
            return cm_copyout(caller()->elf,address,&out,sizeof(out))?-14:0;
        }
        if(caller()->linux_console[fd]) {
            memset(&out,0,sizeof(out));out.mode=0020666;out.nlink=1;out.blksize=4096;
            return cm_copyout(caller()->elf,address,&out,sizeof(out))?-14:0;
        }
        cm_object_t *o=NULL;e=acquire(fd,0,&o);if(e)return e;
        e=cm_vfs_fstat(o->payload,&st);cm_obj_release(o);
    } else {char path[128];e=path_at(dirfd,name,path);if(e)return e;e=cm_vfs_stat(path,&st);}
    if(e)return cm_linux_errno(e);
    convert_stat(&out,&st);return cm_copyout(caller()->elf,address,&out,sizeof(out))?-14:0;
}
static int64_t seek_file(uint64_t fd,uint64_t offset,uint64_t whence)
{
    if(!valid(fd))return -9;
    if(caller()->linux_console[fd] || cm_linux_socket_is(caller(),fd) || cm_linux_pipe_is(caller(),fd))return -29;
    if(whence>2)return -22;
    cm_object_t *o=NULL;int e=acquire(fd,0,&o);if(e)return e;
    int64_t r=cm_vfs_seek(o->payload,(int64_t)offset,(int)whence);cm_obj_release(o);return cm_linux_errno(r);
}
static int64_t getdents(uint64_t fd,uint64_t address,uint64_t capacity)
{
    if(!cm_user_access(caller()->elf,address,capacity,1))return -14;
    if(!capacity)return -22;
    if(cm_linux_socket_is(caller(),fd) || cm_linux_pipe_is(caller(),fd))return -20;
    cm_object_t *o=NULL;int e=acquire(fd,CM_RIGHT_READ,&o);if(e)return e;
    _Alignas(8) uint8_t record[88];uint64_t done=0;
    cm_vfs_lock();
    while(done<capacity) {
        int64_t old=cm_vfs_seek(o->payload,0,1);cm_dirent_t entry;uint64_t next;
        e=cm_vfs_nextdir(o->payload,&entry,&next);
        if(e==CM_ERR_NOENT){e=0;break;}
        if(e)break;
        unsigned n=(unsigned)((19+strlen(entry.name)+1+7)&~7u);
        if(n>capacity-done){(void)cm_vfs_seek(o->payload,old,0);e=done?0:CM_ERR_INVAL;break;}
        memset(record,0,n);cm_linux_dirent64_t *d=(void *)record;
        d->ino=entry.ino;d->off=(int64_t)next;d->reclen=(uint16_t)n;d->type=entry.kind==CM_INO_DIR?4:8;
        memcpy(d->name,entry.name,strlen(entry.name)+1);
        if(cm_copyout(caller()->elf,address+done,record,n)){e=CM_ERR_INVAL;break;}
        done+=n;
    }
    cm_vfs_unlock();cm_obj_release(o);return done?(int64_t)done:cm_linux_errno(e);
}
static int64_t vectors(uint64_t fd,uint64_t address,uint64_t count,int write)
{
    if(count>16)return -22;
    if(cm_linux_socket_kind(caller(),fd)==2){caller()->linux_enosys++;return -38;} /* no fake split UDP datagrams */
    cm_linux_iovec_t vec[16];if(cm_copyin(caller()->elf,vec,address,count*sizeof(vec[0])))return -14;
    uint64_t total=0;
    for(unsigned i=0;i<count;i++) {
        if(vec[i].len>CM_USER_IO_MAX-total || !cm_user_access(caller()->elf,vec[i].base,vec[i].len,!write))return -14;
        total+=vec[i].len;
    }
    int64_t done=0;
    for(unsigned i=0;i<count;i++) {
        int64_t r;
        if(write)r=cm_linux_dispatch(1,fd,vec[i].base,vec[i].len,0);
        else r=read_file(fd,vec[i].base,vec[i].len);
        if(r<0)return done?done:r;
        done+=r;if((uint64_t)r<vec[i].len)break;
    }
    return done;
}
static int64_t fcntl_file(uint64_t fd,uint64_t op,uint64_t arg)
{
    if(!valid(fd))return -9;
    if(op==1)return caller()->linux_cloexec[fd];
    if(op==2){if(arg&~1u)return -22;caller()->linux_cloexec[fd]=(uint8_t)arg;return 0;}
    if(op==0 || op==1030) {
        if(arg>=CM_FD_MAX)return -22;
        unsigned target=(unsigned)arg;while(target<CM_FD_MAX && valid(target))target++;
        if(target==CM_FD_MAX)return -24;
        int64_t r=dup_to(fd,target);if(r>=0)caller()->linux_cloexec[target]=op==1030;return r;
    }
    if(cm_linux_pipe_is(caller(),fd))return cm_linux_pipe_fcntl(caller(),fd,op,arg);
    if(cm_linux_socket_is(caller(),fd))return cm_linux_socket_fcntl(caller(),fd,op,arg);
    if(op==3) {
        if(caller()->linux_console[fd])return caller()->linux_console[fd]==1?0:1;
        cm_object_t *o=NULL;int e=acquire(fd,0,&o);if(e)return e;
        unsigned flags=cm_vfs_file_mode(o->payload);cm_obj_release(o);return flags;
    }
    return -22;
}
#endif
int cm_linux_file_call(uint32_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d,int64_t *result)
{
#if CM_LINUX_ABI
    switch(nr) {
    case 0:*result=read_file(a,b,c);break;
    case 2:*result=open_at(AT_FDCWD,a,b);break;
    case 257:*result=open_at((int)a,b,c);break;
    case 3:*result=close_file(a);break;
    case 8:*result=seek_file(a,b,c);break;
    case 4:case 6:*result=stat_call(0,AT_FDCWD,a,b,0);break;
    case 5:*result=stat_call(a,0,0,b,1);break;
    case 262:
        if(d&~0x1100u){*result=-22;break;}
        if(d&0x1000) {char ch;if(cm_copyin(caller()->elf,&ch,b,1)){*result=-14;break;}
            if(!ch){*result=stat_call(a,0,0,c,1);break;}}
        *result=stat_call(0,(int)a,b,c,0);break;
    case 217:*result=getdents(a,b,c);break;
    case 33:*result=dup_to(a,b);break;
    case 32:*result=fcntl_file(a,0,0);break;
    case 72:*result=fcntl_file(a,b,c);break;
    case 19:case 20:*result=vectors(a,b,c,nr==20);break;
    case 39:*result=(int64_t)cm_linux_group(caller())->pid;break;
    case 110:*result=(int64_t)cm_linux_group(caller())->parent_pid;break;
    case 79: {
        if(b<2){*result=-34;break;}const char root[]="/";
        *result=cm_copyout(caller()->elf,a,root,2)?-14:2;break;
    }
    case 63: {
        cm_linux_utsname_t u={0};memcpy(u.sysname,"CapMix",7);memcpy(u.nodename,"capmix",7);
        memcpy(u.release,"0.8.0-l1",9);memcpy(u.version,"Linux ABI L1 subset",20);memcpy(u.machine,"x86_64",7);
        *result=cm_copyout(caller()->elf,a,&u,sizeof(u))?-14:0;break;
    }
    case 102:case 104:case 107:case 108:*result=0;break; /* fixed identity, not kernel privilege */
    case 186:*result=(int64_t)caller()->pid;break;
    case 218:caller()->linux_clear_tid=a;*result=(int64_t)caller()->pid;break;
    case 16:*result=valid(a)?-25:-9;break; /* ENOTTY, no fake tty ioctl */
    case 231:if(cm_linux_group(caller())->linux_grouped)cm_linux_group_kill(caller(),a&255u);*result=cm_native_exit(a&255u);break;
    default:return 0;
    }
    return 1;
#else
    (void)nr;(void)a;(void)b;(void)c;(void)d;(void)result;return 0;
#endif
}
