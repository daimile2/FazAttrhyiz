#include "capmix/kernel.h"
#include "capmix/control.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/user_shell.inc"
#define CHECK(x) do{if(!(x)){kerr("user control test line %d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
_Static_assert(sizeof(cm_user_spawn_t)==32 && sizeof(cm_user_child_t)==16 && sizeof(cm_user_wait_t)==32 && sizeof(cm_user_boot_t)==56,"control ABI sizes");
int cm_control_selftest(void)
{
    int failed=0;cm_file_t *f=NULL;cm_id_t pid=0,childpid=0;cm_proc_t *p=NULL,*child=NULL;
    uint64_t before=cm_mm_free_pages();
    CHECK(cm_vfs_open("/bin/control-test.elf",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f)==CM_OK);
    CHECK(cm_vfs_write(f,cm_user_shell_elf,sizeof(cm_user_shell_elf))==(int64_t)sizeof(cm_user_shell_elf));
    CHECK(cm_vfs_fsync(f)==CM_OK);cm_vfs_close(f);f=NULL;before=cm_mm_free_pages();
    cm_spawn_attr_t attr={.path="/bin/control-test.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==CM_OK);p=cm_proc_lookup(pid);CHECK(p && p->elf);
    cm_thread_block_prepare(p->main,NULL);cm_sched_set_current(p->main);
    CHECK(cm_syscall(31,2,0x401000,1,0)==CM_ERR_DENIED);
    CHECK(cm_syscall(3,1,UINT64_MAX,UINT64_MAX,0)==CM_ERR_DENIED);
    CHECK(cm_syscall(37,3,0,0,0)==CM_ERR_DENIED);
    kinfo("user control cap denied verified: console/spawn/poweroff default deny");
    CHECK(cm_control_seed(p)==CM_OK);
    uint64_t base=0;for(unsigned i=0;i<p->elf->npages;i++)if(p->elf->pages[i].flags&2){base=p->elf->pages[i].virt;break;}
    CHECK(base);
    CHECK(cm_syscall(32,2,base,0,0)==CM_OK);
    CHECK(cm_syscall(32,2,UINT64_MAX,1,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(31,2,UINT64_MAX,1,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(37,3,1,0,0)==CM_ERR_INVAL); /* never actually shuts down */
    CHECK(cm_syscall(3,1,UINT64_MAX,base,0)==CM_ERR_INVAL);
    cm_user_spawn_t req={.path=base+128,.argv=base+256,.cpu=0};
    uint64_t bad=UINT64_MAX;
    CHECK(cm_copyout(p->elf,base+128,"/bin/hello.elf",15)==CM_OK);
    CHECK(cm_copyout(p->elf,base+256,&bad,8)==CM_OK);
    CHECK(cm_copyout(p->elf,base,&req,sizeof(req))==CM_OK);
    CHECK(cm_syscall(3,1,base,base+64,0)==CM_ERR_INVAL);
    req.argv=0;req.flags=CM_SPAWN_SHELL;
    CHECK(cm_copyout(p->elf,base,&req,sizeof(req))==CM_OK);
    CHECK(cm_syscall(3,1,base,base+64,0)==CM_ERR_DENIED);
    req.flags=0;
    CHECK(cm_copyout(p->elf,base,&req,sizeof(req))==CM_OK);
    CHECK(cm_syscall(3,1,base,base+64,0)==CM_OK);
    cm_user_child_t out;CHECK(cm_copyin(p->elf,&out,base+64,sizeof(out))==CM_OK);
    childpid=out.pid;child=cm_proc_lookup(childpid);CHECK(child && child->parent_pid==pid);
    CHECK(cm_syscall(37,3,0,0,0)==CM_ERR_BUSY);
    CHECK(cm_syscall(33,out.handle,UINT64_MAX,1,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(33,out.handle,base+320,1,0)==CM_ERR_AGAIN);
    int weak=cm_handle_install(p->task,cm_handle_lookup(p->task,out.handle)->obj,0);CHECK(weak>0);
    CHECK(cm_syscall(33,weak,base+320,1,0)==CM_ERR_DENIED);cm_handle_drop(p->task,weak);
    cm_sched_set_current(NULL);
    for(unsigned step=0;step<512 && (!child->exited || (child->main && child->main->id));step++)(void)cm_sched_run_until_idle(1);
    cm_sched_set_current(p->main);
    CHECK(child->exited && (!child->main || !child->main->id));
    CHECK(cm_syscall(33,out.handle,base+320,1,0)==CM_OK);childpid=0;
    cm_user_wait_t result;CHECK(cm_copyin(p->elf,&result,base+320,sizeof(result))==CM_OK && result.pid==out.pid);
#ifdef CAPMIX_HOSTED
    CHECK(result.exit_code==CM_ERR_NOTSUP);
#else
    CHECK(result.exit_code==0);
#endif
    CHECK(!cm_handle_lookup(p->task,out.handle));
    kinfo("user control cap granted verified: nested copyin/out invalid-pointer/weak-right denied child wait/reap consumed handle");
    CHECK(cm_syscall(3,1,base,base+64,0)==CM_OK);
    CHECK(cm_copyin(p->elf,&out,base+64,sizeof(out))==CM_OK);childpid=out.pid;
    cm_sched_set_current(NULL);cm_control_process_exit(p);p->exited=1;p->main->state=CM_TH_DEAD;
    cm_thread_reap(p->main);CHECK(cm_proc_release_elf(pid)==CM_OK);p=NULL;pid=0;
    for(unsigned step=0;step<512 && cm_proc_lookup(childpid);step++) {
        (void)cm_sched_run_until_idle(1);(void)cm_proc_reap_orphans();
    }
    CHECK(!cm_proc_lookup(childpid));childpid=0;
    kinfo("user orphan reclaim verified: parent exit drops child cap; off-CPU child image/task/pages reclaimed");
cleanup:
    cm_sched_set_current(NULL);
    if(f)cm_vfs_close(f);
    if(childpid && child){cm_control_process_exit(child);child->exited=1;
        if(child->main && child->main->id){child->main->state=CM_TH_DEAD;cm_thread_reap(child->main);}
        (void)cm_proc_release_child(pid,childpid);}
    if(p){cm_control_process_exit(p);p->exited=1;p->main->state=CM_TH_DEAD;cm_thread_reap(p->main);(void)cm_proc_release_elf(pid);}
    if(cm_mm_free_pages()!=before){kerr("user control image/child-cap snapshot leak");failed++;}
    kinfo("user control selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
