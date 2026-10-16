#define _POSIX_C_SOURCE 200809L
/* Host-only adapter: reuse the kernel's unchanged COW/VFS implementation on
 * an offline staging image. Only filesystem LBAs may be written. No mkfs. */
#include "capmix/fs.h"
#include "capmix/io.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/obs.h"
#include "capmix/disk_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
static int diskfd=-1;
static uint64_t root;
static unsigned removed;
static int transfer(int fd,void *buf,size_t n,off_t off,int writing)
{
    unsigned char *p=buf;
    while(n){ssize_t r=writing?pwrite(fd,p,n,off):pread(fd,p,n,off);
        if(r<0 && errno==EINTR)continue;
        if(r<=0)return -1;
        p+=r;n-=(size_t)r;off+=r;}
    return 0;
}
cm_error_t cm_blk_rw(uint32_t op,uint64_t lba,uint32_t n,void *buf)
{
    if(!buf || lba<CM_BLK_FS_LBA || lba>=CM_BLK_DISK_SECT || n>CM_BLK_DISK_SECT-lba ||
       (op!=CM_OP_READ && op!=CM_OP_WRITE))return CM_ERR_INVAL;
    return transfer(diskfd,buf,(size_t)n*512,(off_t)((root+lba)*512),op==CM_OP_WRITE)?CM_ERR_IO:CM_OK;
}
cm_error_t cm_blk_flush(void){return fsync(diskfd)?CM_ERR_IO:CM_OK;}
int cm_blk_is_disk(void){return 1;}
cm_error_t cm_blk_volume_blank(int *blank){*blank=0;return CM_OK;} /* never auto-format */
void *cm_page_alloc(unsigned order){if(order>12)return NULL;return calloc(1,(size_t)4096<<order);}
void cm_page_free(void *p,unsigned order){(void)order;free(p);}
int cm_cpu_id(void){return 0;}
uint64_t cm_ticks_now(void){return 0;}
cm_trace_t cm_trace_new(void){return 0;}
void cm_obs_emit(cm_trace_t tr,cm_event_kind_t kind,uint64_t a,uint64_t b,uint64_t c)
{(void)tr;(void)kind;(void)a;(void)b;(void)c;}
int cm_pcache_read(uint32_t blk,void *p){(void)blk;(void)p;return 0;}
void cm_pcache_fill(uint32_t blk,const void *p){(void)blk;(void)p;}
void cm_pcache_invalidate(uint32_t blk){(void)blk;}
void cm_pcache_invalidate_all(void){}
void cm_klog(cm_log_level_t level,const char *fmt,...)
{(void)level;va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);fputc('\n',stderr);}
extern const cm_fs_ops_t *cm_cowfs_ops(void);

/* Only one fixed namespace may be removed; no user-controlled traversal root. */
static int purge(const char *path,unsigned depth)
{
    if(depth>32 || removed>=512)return CM_ERR_NOSPC;
    cm_stat_t st;int e=cm_vfs_stat(path,&st);if(e==CM_ERR_NOENT)return 0;if(e)return e;
    if(st.kind==CM_INO_DIR) {
        cm_dirent_t de;
        while((e=cm_vfs_readdir(path,0,&de))==0) {
            if(!de.name[0] || !strcmp(de.name,".") || !strcmp(de.name,"..") || strchr(de.name,'/'))return CM_ERR_INVAL;
            char child[128];int n=snprintf(child,sizeof(child),"%s/%s",path,de.name);
            if(n<0 || (unsigned)n>=sizeof(child))return CM_ERR_NOSPC;
            e=purge(child,depth+1);if(e)return e;
        }
        if(e!=CM_ERR_NOENT)return e;
    } else if(st.kind!=CM_INO_FILE)return CM_ERR_NOTSUP;
    if(removed>=512)return CM_ERR_NOSPC;
    e=cm_vfs_unlink(path);if(!e)removed++;return e;
}
static int directory(const char *path)
{
    cm_stat_t st;int e=cm_vfs_stat(path,&st);
    return e==CM_ERR_NOENT?cm_vfs_mkdir(path):e?e:st.kind==CM_INO_DIR?0:CM_ERR_NOTDIR;
}
int main(int argc,char **argv)
{
    if(argc!=3 || (strcmp(argv[2],"prepare") && strcmp(argv[2],"purge") && strcmp(argv[2],"status"))) {
        fprintf(stderr,"usage: linux-package STAGING_IMAGE prepare|purge|status\n");return 1;
    }
    diskfd=open(argv[1],O_RDWR|O_NOFOLLOW);struct stat ds;unsigned char sector[512];int e=CM_ERR_INVAL;
    if(diskfd<0 || fstat(diskfd,&ds) || !S_ISREG(ds.st_mode) || ds.st_nlink!=1 || ds.st_size%512 || transfer(diskfd,sector,512,0,0))goto done;
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET};
    if(flock(diskfd,LOCK_EX|LOCK_NB) || fcntl(diskfd,F_SETLK,&lock))goto done;
    e=cm_disk_layout_parse(sector,(uint64_t)ds.st_size/512,&root);if(e)goto done;
    e=cm_cow_mount();if(e)goto done; /* never format */
    e=cm_vfs_mount("/",cm_cowfs_ops(),NULL);if(e)goto done;
    if(!strcmp(argv[2],"prepare")){e=directory("/data");if(!e)e=directory("/data/linux");}
    else if(!strcmp(argv[2],"purge"))e=purge("/data/linux",0);
    if(e)goto done;
    e=cm_vfs_sync();if(e)goto done;
    e=cm_vfs_umount("/");if(e)goto done;
    e=cm_cow_mount();if(!e)e=cm_vfs_mount("/",cm_cowfs_ops(),NULL);if(e)goto done;
    cm_stat_t st;e=cm_vfs_stat("/data/linux",&st);
    int present=e==0;
    if(e==CM_ERR_NOENT)e=0;
    if(!e && !strcmp(argv[2],"prepare") && (!present || st.kind!=CM_INO_DIR))e=CM_ERR_IO;
    if(!e && !strcmp(argv[2],"purge") && present)e=CM_ERR_IO;
    if(!e)printf("linux package data: present=%d removed=%u remount=verified\n",present,removed);
done:
    if(diskfd>=0)close(diskfd);
    if(e)fprintf(stderr,"linux package COW refused: error=%d; staged changes only\n",e);
    return e?1:0;
}
