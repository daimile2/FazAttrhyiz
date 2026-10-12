#include "capmix/config.h"
#include "capmix/net.h"
#include "capmix/boot.h"
#include "capmix/shell.h"
#include "capmix/kernel.h"
#include "capmix/console.h"
#include "capmix/fs.h"
#include "capmix/io.h"
#include <string.h>
#include "../build/hello_elf.inc"
#include "../build/file_elf.inc"
#include "../build/wire_elf.inc"
static cm_error_t root_status;
void cm_product_record_root(cm_error_t status) {root_status=status;}
#ifndef CAPMIX_HOSTED
static cm_error_t install_missing(const char *path,const void *bytes,uint64_t length)
{
    cm_stat_t stat;cm_error_t e=cm_vfs_stat(path,&stat);
    if(!e)return CM_OK;
    if(e!=CM_ERR_NOENT)return e;
    cm_file_t *file=NULL;e=cm_vfs_open(path,CM_O_WRONLY|CM_O_CREAT,&file);
    if(e)return e;
    int64_t n=cm_vfs_write(file,bytes,length);
    e=n==(int64_t)length?cm_vfs_fsync(file):CM_ERR_IO;
    cm_vfs_close(file);
    if(!e)kinfo("init: provisioned %s via VFS (%u bytes)",path,(unsigned)length);
    return e;
}
static cm_error_t provision(void)
{
    cm_stat_t stat;cm_error_t e=cm_vfs_stat("/bin",&stat);
    if(e==CM_ERR_NOENT)e=cm_vfs_mkdir("/bin");
    else if(!e && stat.kind!=CM_INO_DIR)e=CM_ERR_NOTDIR;
    if(e)return e;
    e=install_missing("/bin/hello.elf",cm_test_elf,sizeof(cm_test_elf));
    if(!e)e=install_missing("/bin/filetest.elf",cm_file_elf,sizeof(cm_file_elf));
    return e?e:install_missing("/bin/udpwire.elf",cm_wire_elf,sizeof(cm_wire_elf));
}
static void init_entry(void)
{
    cm_proc_t *p=cm_proc_current();
    cm_iface_t *nic=cm_net_primary_iface();
    kinfo("init: CapMix %s mode=product root=%s nic=%s/%s", CAPMIX_VERSION,
          cm_blk_is_disk()?"disk":"ram",nic?nic->name:"unavailable",nic?(nic->up?"up":"down"):"none");
    kinfo("init: pid=%llu product root=%s COW (no phase pipeline)",(unsigned long)p->pid,
          cm_blk_backend_name());
    cm_error_t e=provision();if(e)kerr("init: ELF provisioning failed=%d",e);
    cm_shell_run();cm_syscall(CM_SYS_EXIT,0,0,0,0);
}
#endif
void cm_product_start(cm_error_t status)
{
#ifdef CAPMIX_HOSTED
    CM_UNUSED(status);kinfo("init: hosted product runner skipped");
#else
    if(status || root_status) {
        kerr("init: root mount failed=%d, product refused",status?status:root_status);cm_console_idle();return;
    }
    cm_smp_sched_disable(); /* native PID1 and one ELF stay on BSP */
    cm_spawn_attr_t attr={.path="init",.entry=init_entry,.qos=CM_QOS_USER_INTERACTIVE};
    cm_id_t pid=0;cm_error_t e=cm_posix_spawn(&attr,&pid);
    if(e || pid!=1){kerr("init: PID1 spawn failed=%d pid=%llu",e,(unsigned long)pid);cm_console_idle();return;}
    (void)cm_net_bg_start();
    cm_proc_t *p=cm_proc_lookup(pid);
    while(!p->exited || (p->main && p->main->id)) {
        (void)cm_sched_run_until_idle(1);
        __asm__ volatile("sti; hlt" ::: "memory");
    }
    cm_net_bg_stop();
    kinfo("init: PID1 exited; kernel console idle");cm_console_idle();
#endif
}
