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
int main(int argc,char **argv)
{
    if(argc!=4){fprintf(stderr,"usage: cow-put STAGING_IMAGE SOURCE /absolute/destination\n");return 1;}
    diskfd=open(argv[1],O_RDWR|O_NOFOLLOW);int source=open(argv[2],O_RDONLY);
    struct stat ds,ss;unsigned char sector[512];int e=CM_ERR_INVAL;cm_file_t *file=NULL;
    unsigned char *bytes=NULL,*check=NULL;
    if(diskfd<0 || source<0 || fstat(diskfd,&ds) || fstat(source,&ss) ||
       !S_ISREG(ds.st_mode) || !S_ISREG(ss.st_mode) || ss.st_size<=0 || ss.st_size>1024*1024 ||
       ds.st_size%512 || transfer(diskfd,sector,512,0,0))goto done;
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=0,.l_len=0};
    if(ds.st_nlink!=1 || flock(diskfd,LOCK_EX|LOCK_NB) || fcntl(diskfd,F_SETLK,&lock))goto done;
    e=cm_disk_layout_parse(sector,(uint64_t)ds.st_size/512,&root);if(e)goto done;
    if(argv[3][0]!='/' || strlen(argv[3])>=128){e=CM_ERR_INVAL;goto done;}
    /* Mount only: do not use cm_vfs_init() which can initialize a blank root. */
    e=cm_cow_mount();if(e)goto done;
    e=cm_vfs_mount("/",cm_cowfs_ops(),NULL);if(e)goto done;
    cm_stat_t existing;e=cm_vfs_stat(argv[3],&existing);
    if(!e){e=CM_ERR_EXIST;goto done;}if(e!=CM_ERR_NOENT)goto done;
    bytes=malloc((size_t)ss.st_size);check=malloc((size_t)ss.st_size);
    if(!bytes || !check){e=CM_ERR_NOMEM;goto done;}
    if(transfer(source,bytes,(size_t)ss.st_size,0,0)){e=CM_ERR_IO;goto done;}
    e=cm_vfs_open(argv[3],CM_O_CREAT|CM_O_WRONLY,&file);if(e)goto done;
    if(cm_vfs_write(file,bytes,(uint64_t)ss.st_size)!=ss.st_size){e=CM_ERR_IO;goto done;}
    e=cm_vfs_fsync(file);cm_vfs_close(file);file=NULL;if(e)goto done;
    e=cm_vfs_umount("/");if(e)goto done;
    e=cm_cow_mount();if(e)goto done;
    e=cm_vfs_mount("/",cm_cowfs_ops(),NULL);if(e)goto done;
    e=cm_vfs_open(argv[3],CM_O_RDONLY,&file);if(e)goto done;
    if(cm_vfs_read(file,check,(uint64_t)ss.st_size)!=ss.st_size || memcmp(bytes,check,(size_t)ss.st_size))e=CM_ERR_IO;
    cm_vfs_close(file);file=NULL;if(!e)e=cm_vfs_sync();
    if(!e)printf("cow copy: %s bytes=%lld remount/readback verified\n",argv[3],(long long)ss.st_size);
done:
    if(file)cm_vfs_close(file);
    free(bytes);free(check);if(source>=0)close(source);if(diskfd>=0)close(diskfd);
    if(e)fprintf(stderr,"cow copy refused/failed: error=%d; staging image not published\n",e);
    return e?1:0;
}
