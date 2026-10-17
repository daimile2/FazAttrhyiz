#include "capmix/control.h"
#include "capmix/elf.h"
#include "capmix/disk_layout.h"
#include "capmix/config.h"
#include "capmix/net.h"
#include "capmix/dhcp.h"
#include "capmix/boot.h"
#include "capmix/shell.h"
#include "capmix/kernel.h"
#include "capmix/console.h"
#include "capmix/fs.h"
#include "capmix/io.h"
#include <string.h>
#include "../build/user_init.inc"
#include "../build/user_shell.inc"
#include "../build/spin_elf.inc"
#include "../build/hello_elf.inc"
#include "../build/file_elf.inc"
#include "../build/wire_elf.inc"
#include "../build/tcp_elf.inc"
#include "../build/argv_elf.inc"
#include "../build/concurrent_elf.inc"
#include "../build/example_hello.inc"
#include "../build/example_nc.inc"
#include "../build/example_cat.inc"
#include "../build/example_ls.inc"
#include "../build/example_http-get.inc"
#include "../build/example_httpd.inc"
#include "../build/linux_hello.inc"
#include "../build/busybox_l1.inc"
#include "../build/linux_l3.inc"
#include "../build/busybox_sh.inc"
#include "../build/micropython.inc"
#include "../build/micropython_demo.inc"
#include "../build/linux_sh_demo.inc"
#include "../build/linux_l5.inc"
#include "../build/linux_l5_musl.inc"
#include "../build/linux_l4_main.inc"
#include "../build/linux_l4_interp.inc"
#include "../build/ldso_main.inc"
#include "../build/ldso_interp.inc"
#include "../build/ldso_lib.inc"
#include "../build/tls_main.inc"
#include "../build/tls_a.inc"
#include "../build/tls_b.inc"
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
    e=install_missing("/bin/shell.elf",cm_user_shell_elf,sizeof(cm_user_shell_elf));
    if(!e)e=install_missing("/bin/spin.elf",cm_spin_elf,sizeof(cm_spin_elf));
    if(!e)e=install_missing("/bin/hello.elf",cm_test_elf,sizeof(cm_test_elf));
    if(!e)e=install_missing("/bin/filetest.elf",cm_file_elf,sizeof(cm_file_elf));
    if(!e)e=install_missing("/bin/udpwire.elf",cm_wire_elf,sizeof(cm_wire_elf));
    if(!e)e=install_missing("/bin/tcpwire.elf",cm_tcp_elf,sizeof(cm_tcp_elf));
    if(!e)e=install_missing("/bin/argtest.elf",cm_argv_elf,sizeof(cm_argv_elf));
    if(!e)e=install_missing("/bin/concurrent.elf",cm_concurrent_elf,sizeof(cm_concurrent_elf));
    if(!e)e=install_missing("/bin/hello-sdk.elf",cm_example_hello_elf,sizeof(cm_example_hello_elf));
    if(!e)e=install_missing("/bin/nc.elf",cm_example_nc_elf,sizeof(cm_example_nc_elf));
    if(!e)e=install_missing("/bin/cat.elf",cm_example_cat_elf,sizeof(cm_example_cat_elf));
    if(!e)e=install_missing("/bin/ls.elf",cm_example_ls_elf,sizeof(cm_example_ls_elf));
    if(!e)e=install_missing("/bin/http-get.elf",cm_example_http_get_elf,sizeof(cm_example_http_get_elf));
    if(!e)e=install_missing("/bin/httpd.elf",cm_example_httpd_elf,sizeof(cm_example_httpd_elf));
    if(!e && cm_linux_enabled())e=install_missing("/bin/linux-hello.elf",cm_linux_hello_elf,sizeof(cm_linux_hello_elf));
    if(!e && cm_linux_enabled())e=install_missing("/bin/busybox-l1.elf",cm_busybox_l1_elf,sizeof(cm_busybox_l1_elf));
    if(!e && cm_linux_enabled())e=install_missing("/bin/linux-net.elf",cm_linux_l3_elf,sizeof(cm_linux_l3_elf));
    if(!e && cm_linux_enabled())e=install_missing("/bin/busybox-sh.elf",cm_busybox_sh_elf,sizeof(cm_busybox_sh_elf));
    if(!e && cm_linux_enabled())e=install_missing("/bin/micropython.elf",cm_micropython_elf,sizeof(cm_micropython_elf));
    if(!e && cm_linux_enabled())e=install_missing("/bin/linux-mmap.elf",cm_linux_l5_elf,sizeof(cm_linux_l5_elf));
    if(!e && cm_linux_enabled())e=install_missing("/bin/musl-mmap.elf",cm_linux_l5_musl,sizeof(cm_linux_l5_musl));
    if(!e && cm_linux_interp_enabled()) {
        cm_stat_t lib;e=cm_vfs_stat("/lib",&lib);
        if(e==CM_ERR_NOENT)e=cm_vfs_mkdir("/lib");
        if(!e)e=install_missing(CM_ELF_INTERP_PATH,cm_linux_l4_interp,sizeof(cm_linux_l4_interp));
        if(!e)e=install_missing(CM_ELF_LDSO_PATH,cm_ldso_interp,sizeof(cm_ldso_interp));
        if(!e)e=install_missing("/lib/libcmgold.so.1",cm_ldso_lib,sizeof(cm_ldso_lib));
        if(!e)e=install_missing("/bin/linux-dynamic.elf",cm_ldso_main,sizeof(cm_ldso_main));
        if(!e)e=install_missing("/lib/libcm_tls_a.so.1",cm_tls_a,sizeof(cm_tls_a));
        if(!e)e=install_missing("/lib/libcm_tls_b.so.1",cm_tls_b,sizeof(cm_tls_b));
        if(!e)e=install_missing("/bin/linux-tls.elf",cm_tls_main,sizeof(cm_tls_main));
        if(!e)e=install_missing("/bin/linux-l4a.elf",cm_linux_l4_main,sizeof(cm_linux_l4_main));
    }
    if(!e && cm_linux_enabled()) {
        const char *dirs[]={"/data","/data/linux"};
        for(unsigned i=0;i<2 && !e;i++) {
            cm_stat_t data;e=cm_vfs_stat(dirs[i],&data);
            if(e==CM_ERR_NOENT)e=cm_vfs_mkdir(dirs[i]);
            else if(!e && data.kind!=CM_INO_DIR)e=CM_ERR_NOTDIR;
        }
        if(!e)e=install_missing("/data/linux/micropython-demo.py",cm_micropython_demo,sizeof(cm_micropython_demo));
        if(!e)e=install_missing("/data/linux/sh-smoke.sh",cm_linux_sh_demo,sizeof(cm_linux_sh_demo));
        if(!e)kinfo("linux package ready: /data/linux retained; persona enabled; uninstall via offline profile (no hot unload)");
    }
    static const char sample[]="CapMix examples: persistent root ready\n";
    if(!e)e=install_missing("/example.txt",sample,sizeof(sample)-1);
    return e;
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
    cm_smp_sched_disable(); /* publish PID1 and worker before opening AP scheduling */
    /* Debug creates lo in its network test; product must not depend on that test. */
    if(cm_linux_enabled() && cm_if_find("lo")<0 && cm_if_add("lo",CM_UDP_LO_ADDR)<0) {
        kerr("init: Linux UDP loopback unavailable (interface table full)");
    }
    cm_error_t prep=provision();
    cm_stat_t st;
    if(!prep){prep=cm_vfs_stat("/sbin",&st);if(prep==CM_ERR_NOENT)prep=cm_vfs_mkdir("/sbin");}
    if(!prep)prep=install_missing("/sbin/init.elf",cm_user_init_elf,sizeof(cm_user_init_elf));
    if(prep){kerr("init: bootstrap ELF provisioning failed=%d",prep);cm_console_idle();return;}
    const char *argv[]={"/sbin/init.elf",NULL};
    const char *envp[]={"PATH=/bin","CAPMIX=product",NULL};
    cm_spawn_attr_t attr={.path="/sbin/init.elf",.argv=argv,.envp=envp,.qos=CM_QOS_USER_INTERACTIVE};
    cm_id_t pid=0;cm_error_t e=cm_posix_spawn(&attr,&pid);
    if(e || pid!=1){kerr("init: PID1 spawn failed=%d pid=%llu",e,(unsigned long)pid);cm_console_idle();return;}
    cm_proc_t *p=cm_proc_lookup(pid);
    e=cm_control_seed(p);
    if(e){cm_control_process_exit(p);p->exited=1;p->main->state=CM_TH_DEAD;cm_thread_reap(p->main);
        (void)cm_proc_release_child(0,pid);kerr("init: bootstrap capabilities failed=%d",e);cm_console_idle();return;}
    if(cm_blk_installed())kinfo("disk install: system ELF provisioning synced; root=disk base_lba=%llu ready",(unsigned long)cm_blk_root_base());
    kinfo("linux persona: compiled=%u enabled=%u product default=off native files retained",CM_LINUX_ABI,cm_linux_enabled());
    kinfo("init: kernel bootstrap -> /sbin/init.elf PID1 (user ELF)");
    (void)cm_net_bg_start();
    cm_smp_sched_enable();
    for(;;) {
        int orphans=cm_proc_reap_orphans();
        if(__atomic_load_n(&p->exited,__ATOMIC_ACQUIRE) && (!p->main || !p->main->id) && !orphans)break;
        (void)cm_sched_run_until_idle(1);
        __asm__ volatile("sti; hlt" ::: "memory");
    }
    cm_smp_sched_disable();
    cm_net_bg_stop();
    if(p->elf)(void)cm_proc_release_child(0,pid);
    kinfo("init: PID1 exited; kernel console idle");cm_console_idle();
#endif
}
