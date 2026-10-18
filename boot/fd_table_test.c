#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include "capmix/abi.h"
#include "capmix/cap_internal.h"
#include <string.h>
#define CHECK(x) do{if(!(x)){kerr("fd64 line=%d linux=%d: %s",__LINE__,linux,#x);failed++;goto cleanup;}}while(0)
static unsigned handles(cm_proc_t *p)
{
    unsigned count=0;uint64_t irq=cm_cap_lock();
    for(unsigned i=1;i<CM_HANDLES_PER_TASK;i++)count+=cm_handle_lookup(p->task,(int)i)!=NULL;
    cm_cap_unlock(irq);return count;
}
static int exercise(int linux)
{
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL,*child=NULL;cm_id_t child_id=0;
    uint64_t irq=cm_irq_save(),commit,limit;cm_commit_stats(&commit,&limit);
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages();unsigned files=cm_vfs_open_files();
    cm_spawn_attr_t a={.path=linux?"/bin/linux-hello.elf":"/bin/hello.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(!cm_posix_spawn(&a,&pid));p=cm_proc_lookup(pid);CHECK(p);cm_sched_set_current(p->main);
    uint64_t u=0x7fc000;CHECK(!cm_copyout(p->elf,u,"/fd-table.txt",14));
    int64_t fd=linux?cm_linux_dispatch(2,u,2,0,0):cm_native_open_path("/fd-table.txt",CM_O_RDWR);
    CHECK(fd==3);
    for(int i=4;i<CM_FD_MAX;i++)CHECK((linux?cm_linux_dispatch(33,3,i,0,0):cm_native_dup2(3,i))==i);
    unsigned held=handles(p),opened=cm_vfs_open_files();
    CHECK(opened==files+1);
    CHECK((linux?cm_linux_dispatch(2,u,0,0,0):cm_native_open_path("/fd-table.txt",0))==(linux?-24:CM_ERR_NOSPC));
    CHECK(handles(p)==held && cm_vfs_open_files()==opened);
    CHECK((linux?cm_linux_dispatch(0,CM_FD_MAX-1,u+64,1,0):cm_native_read(CM_FD_MAX-1,u+64,1))==1);
    char byte;CHECK(!cm_copyin(p->elf,&byte,u+64,1) && byte=='f');
    CHECK(!cm_copyout(p->elf,u+65,"d",1));
    CHECK((linux?cm_linux_dispatch(1,CM_FD_MAX-1,u+65,1,0):cm_native_write(CM_FD_MAX-1,u+65,1))==1);
    if(linux) {
        CHECK(cm_linux_dispatch(72,CM_FD_MAX-1,2,1,0)==0 && p->linux_cloexec[CM_FD_MAX-1]);
        cm_linux_regs_t frame={0};p->linux_frame=&frame;
        int64_t forked=cm_linux_fork();CHECK(forked>0);child_id=(cm_id_t)forked;
        child=cm_proc_lookup(child_id);CHECK(child && child->fd[CM_FD_MAX-1]>=0 && child->linux_cloexec[CM_FD_MAX-1]==1);
        cm_proc_finish_exit(child,0);cm_thread_reap(child->main);
        CHECK(!cm_proc_release_child(pid,child_id));child=NULL;child_id=0;p->linux_frame=NULL;
        CHECK(p->fd[CM_FD_MAX-1]>=0 && p->linux_cloexec[CM_FD_MAX-1]==1);
        CHECK(cm_linux_dispatch(72,3,0,3,0)==-24); /* F_DUPFD full */
        CHECK(cm_linux_dispatch(72,3,0,CM_FD_MAX,0)==-22);
        CHECK(cm_linux_dispatch(33,3,CM_FD_MAX,0,0)==-9);
    } else CHECK(cm_native_dup2(3,CM_FD_MAX)==CM_ERR_INVAL && cm_native_dup2(3,-1)==CM_ERR_INVAL);
    CHECK((linux?cm_linux_dispatch(3,47,0,0,0):cm_native_close(47))==0);
    CHECK((linux?cm_linux_dispatch(2,u,0,0,0):cm_native_open_path("/fd-table.txt",0))==47);
    CHECK((linux?cm_linux_dispatch(3,CM_FD_MAX-1,0,0,0):cm_native_close(CM_FD_MAX-1))==0);
    CHECK((linux?cm_linux_dispatch(2,u,0,0,0):cm_native_open_path("/fd-table.txt",0))==CM_FD_MAX-1);
    if(linux)CHECK(!p->linux_cloexec[CM_FD_MAX-1] && !p->linux_console[CM_FD_MAX-1]);
    for(int i=3;i<CM_FD_MAX;i++)CHECK((linux?cm_linux_dispatch(3,i,0,0,0):cm_native_close(i))==0);
    CHECK(cm_vfs_open_files()==files);
    CHECK((linux?cm_linux_dispatch(2,u,0,0,0):cm_native_open_path("/fd-table.txt",0))==3);
    kinfo("fd64 %s: slots=%u filled=%u full=%s rollback=ok high-fd=%u hole=47 reuse=3 verified",
        linux?"Linux":"native",CM_FD_MAX,CM_FD_MAX-3,linux?"EMFILE":"NOSPC",CM_FD_MAX-1);
cleanup:
    cm_sched_set_current(NULL);
    if(child){cm_proc_finish_exit(child,137);if(child->main && child->main->id)cm_thread_reap(child->main);if(cm_proc_release_child(pid,child_id))failed++;}
    if(p)p->linux_frame=NULL;
    if(p){cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    cm_irq_restore(irq);
    uint64_t after;cm_commit_stats(&after,&limit);
    if(after!=commit || cm_mm_free_pages()+cm_file_cache_pages()!=pages || cm_vfs_open_files()!=files){kerr("fd64 reclaim mismatch linux=%d",linux);failed++;}
    return failed;
}
int cm_fd_table_selftest(void)
{
    cm_file_t *f=NULL;int e=cm_vfs_open("/fd-table.txt",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);
    if(e)return 1;
    if(cm_vfs_write(f,"fd64",4)!=4)e=1;
    cm_vfs_close(f);if(e)return 1;
    int failed=exercise(0);
    if(cm_linux_enabled())failed+=exercise(1);
    else kinfo("fd64 Linux skipped: persona disabled; native tested");
    kinfo("fd64 metadata: proc_bytes=%u slots=%u fixed_pool=%u delta=192 bytes/slot,6144 bytes/pool",(unsigned)sizeof(cm_proc_t),CM_FD_MAX,CM_MAX_PROCS);
    kinfo("fd64 selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
