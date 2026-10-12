#include "capmix/persist.h"
#include "capmix/fs.h"
#include "capmix/io.h"
#include "capmix/virtio_blk.h"
#include "capmix/klog.h"
#include <string.h>
static int persistent,failures;
static uint8_t expected[4097],readback[4097];
static const char path[]="/m3-persist.bin";
static cm_error_t verify(void)
{
    cm_file_t *file=NULL;
    cm_error_t e=cm_vfs_open(path,CM_O_RDONLY,&file);
    if(e)return e;
    cm_stat_t stat;
    e=cm_vfs_stat(path,&stat);
    int64_t n=e?e:cm_vfs_read(file,readback,sizeof(readback));
    cm_vfs_close(file);
    return !e && stat.size==sizeof(expected) && n==(int64_t)sizeof(readback) &&
           !memcmp(expected,readback,sizeof(expected)) ? CM_OK:CM_ERR_IO;
}
static cm_error_t write_file(void)
{
    cm_file_t *file=NULL;
    cm_error_t e=cm_vfs_open(path,CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&file);
    if(e)return e;
    int64_t n=cm_vfs_write(file,expected,sizeof(expected));
    e=n==(int64_t)sizeof(expected)?cm_vfs_fsync(file):CM_ERR_IO;
    cm_vfs_close(file);return e;
}
void cm_m3_prepare(cm_error_t mount_result)
{
    persistent=cm_blk_backend();
    if(!persistent) {
        kinfo("m3 persist skipped: no supported disk; ramdisk root unchanged");return;
    }
    for(unsigned i=0;i<sizeof(expected);i++)expected[i]=(uint8_t)(37*i+11);
    static const char marker[]="CapMix M3 persistent root v1";
    memcpy(expected,marker,sizeof(marker)-1);
    cm_error_t e=mount_result;
    if(!e) {
        kinfo("m3 mount: '/' on %s, destructive legacy tests will use scratch ramdisk",cm_blk_backend_name());
        e=verify();
        if(!e)kinfo("m3 persist previous boot verified: %s bytes=%u",path,(unsigned)sizeof(expected));
        else if(e==CM_ERR_NOENT) {kinfo("m3 persist first boot: creating marker");e=CM_OK;}
        if(!e)e=write_file();
        if(!e)e=cm_vfs_sync();
        if(!e)e=cm_cow_mount(); /* drops inode/indirect/page-cache state, reloads real sectors */
        if(!e)e=verify();
        if(!e)kinfo("m3 persist: write -> sync/FLUSH -> remount -> read verified bytes=%u virtio_requests=%llu virtio_flushes=%llu",
                    (unsigned)sizeof(expected),(unsigned long)cm_virtio_blk_requests(),(unsigned long)cm_virtio_blk_flushes());
    }
    if(e) {kerr("m3 persist prepare failed: %d (no unknown-volume format)",e);failures++;}
    /* No open files may survive a backend/root change. Scratch disk keeps all
     * phase3 raw-pointer corruption and phase4 offline/crash semantics unchanged. */
    if(cm_vfs_open_files() || (!mount_result && cm_vfs_umount("/")!=CM_OK) ||
       cm_blk_select(0)!=CM_OK || cm_vfs_init()!=CM_OK) {
        kerr("m3 scratch root switch failed");failures++;
    } else kinfo("m3 mount: scratch ramdisk '/' for phase0-9 tests");
}
int cm_m3_finish(void)
{
    if(!persistent)return 0;
    cm_error_t e=cm_vfs_open_files()?CM_ERR_BUSY:cm_vfs_umount("/");
    if(!e)e=cm_blk_select_backend(persistent);
    if(!e)e=cm_vfs_init();
    if(!e)e=verify();
    if(e) {kerr("m3 persist root restore failed: %d",e);failures++;}
    else kinfo("m3 mount restored: '/' %s persist verified; scratch offline state isolated virtio_requests=%llu virtio_flushes=%llu",cm_blk_backend_name(),
               (unsigned long)cm_virtio_blk_requests(),(unsigned long)cm_virtio_blk_flushes());
    kinfo("m3 persist selftest: %s (%d failures)",failures?"FAIL":"PASS",failures);
    return failures;
}
