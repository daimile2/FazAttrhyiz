#include "capmix/kernel.h"
#include "capmix/control.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/user_shell.inc"
#include "../build/spin_elf.inc"
#include "../build/example_cat.inc"
#define CHECK(x) do{if(!(x)){kerr("user control test line %d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
_Static_assert(sizeof(cm_user_stdio_t)==16 && sizeof(cm_user_spawn_t)==32 && sizeof(cm_user_child_t)==16 && sizeof(cm_user_wait_t)==32 && sizeof(cm_user_boot_t)==56,"control ABI sizes");
int cm_control_selftest(void)
{
    int failed=0;cm_file_t *f=NULL;cm_id_t pid=0,childpid=0;cm_proc_t *p=NULL,*child=NULL;
    /* File writes legitimately retain clean cache pages. Account them exactly
     * while keeping process/FILE resources subject to no-leak snapshots. */
    uint64_t before=cm_mm_free_pages()+cm_file_cache_pages();
    CHECK(cm_vfs_open("/bin/spin.elf",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f)==CM_OK);
    CHECK(cm_vfs_write(f,cm_spin_elf,sizeof(cm_spin_elf))==(int64_t)sizeof(cm_spin_elf));
    CHECK(cm_vfs_fsync(f)==CM_OK);cm_vfs_close(f);f=NULL;
    CHECK(cm_vfs_open("/bin/control-test.elf",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f)==CM_OK);
    CHECK(cm_vfs_write(f,cm_user_shell_elf,sizeof(cm_user_shell_elf))==(int64_t)sizeof(cm_user_shell_elf));
    CHECK(cm_vfs_fsync(f)==CM_OK);cm_vfs_close(f);f=NULL;before=cm_mm_free_pages()+cm_file_cache_pages();
    CHECK(cm_vfs_open("/bin/stdio-cat.elf",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f)==CM_OK);
    CHECK(cm_vfs_write(f,cm_example_cat_elf,sizeof(cm_example_cat_elf))==(int64_t)sizeof(cm_example_cat_elf));
    CHECK(cm_vfs_fsync(f)==CM_OK);cm_vfs_close(f);f=NULL;before=cm_mm_free_pages()+cm_file_cache_pages();
    cm_spawn_attr_t attr={.path="/bin/control-test.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==CM_OK);p=cm_proc_lookup(pid);CHECK(p && p->elf);
    cm_thread_block_prepare(p->main,NULL);cm_sched_set_current(p->main);
    CHECK(cm_syscall(31,2,0x401000,1,0)==CM_ERR_DENIED);
    CHECK(cm_syscall(3,1,UINT64_MAX,UINT64_MAX,0)==CM_ERR_DENIED);
    CHECK(cm_syscall(37,3,0,0,0)==CM_ERR_DENIED);
    CHECK(cm_syscall(CM_SYS_CHILD_KILL,3,0,0,0)==CM_ERR_DENIED);
    CHECK(cm_syscall(CM_SYS_TSC_HZ,1,0,0,0)==CM_ERR_INVAL);
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
#ifdef CAPMIX_HOSTED
    CHECK(cm_syscall(37,3,CM_POWEROFF_FORCE,0,0)==CM_OK);
    CHECK(child && !child->exited && child->elf && child->main->id);
    kinfo("user shutdown selftest: force with live child retains resources, sync skipped (hosted no halt)");
#endif
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
    /* FILE references survive parent close. Child receives only READ/WRITE,
     * and bound fd1 must never fall through to the diagnostic console. */
    const char payload[]="stdio redirected payload verified\n";
    CHECK(cm_vfs_open("/stdio-in.txt",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f)==CM_OK);
    CHECK(cm_vfs_write(f,payload,sizeof(payload)-1)==(int64_t)sizeof(payload)-1);
    CHECK(cm_vfs_fsync(f)==CM_OK);cm_vfs_close(f);f=NULL;
    uint32_t files_before=cm_vfs_open_files();uint64_t stdio_before=cm_mm_free_pages()+cm_file_cache_pages();
    CHECK(cm_copyout(p->elf,base+1024,"/stdio-in.txt",14)==CM_OK);
    CHECK(cm_copyout(p->elf,base+1088,"/stdio-out.txt",15)==CM_OK);
    int input=(int)cm_syscall(11,base+1024,CM_O_RDONLY,0,0);CHECK(input>=3);
    int output=(int)cm_syscall(11,base+1088,CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,0,0);CHECK(output>=3);
    CHECK(cm_copyout(p->elf,base+128,"/bin/stdio-cat.elf",sizeof("/bin/stdio-cat.elf"))==CM_OK);
    cm_user_stdio_t io={.size=sizeof(io),.input_fd=input,.output_fd=output};
    CHECK(cm_copyout(p->elf,base+512,&io,sizeof(io))==CM_OK);
    CHECK(cm_syscall(3,1,base,base+64,UINT64_MAX)==CM_ERR_INVAL);
    cm_handle_t *input_cap=cm_handle_lookup(p->task,cm_fdio_lookup(p,input));CHECK(input_cap);
    cm_rights_t input_rights=input_cap->rights;input_cap->rights&=~CM_RIGHT_DUPLICATE;
    int64_t denied_stdio=cm_syscall(3,1,base,base+64,base+512);input_cap->rights=input_rights;
    CHECK(denied_stdio==CM_ERR_DENIED);
    io.input_fd=output;CHECK(cm_copyout(p->elf,base+512,&io,sizeof(io))==CM_OK);
    CHECK(cm_syscall(3,1,base,base+64,base+512)==CM_ERR_DENIED);
    io.input_fd=input;CHECK(cm_copyout(p->elf,base+512,&io,sizeof(io))==CM_OK);
    CHECK(cm_syscall(3,1,base,base+64,base+512)==CM_OK);
    CHECK(cm_copyin(p->elf,&out,base+64,sizeof(out))==CM_OK);
    childpid=out.pid;child=cm_proc_lookup(childpid);CHECK(child && child->stdio_mask==3);
    CHECK(cm_handle_lookup(child->task,child->fd[0])->rights==CM_RIGHT_READ);
    CHECK(cm_handle_lookup(child->task,child->fd[1])->rights==CM_RIGHT_WRITE);
    CHECK(cm_syscall(13,input,0,0,0)==CM_OK);
    CHECK(cm_vfs_open_files()==files_before+2); /* source is still held by child */
    cm_sched_set_current(child->main);
    uint64_t cb=0;for(unsigned i=0;i<child->elf->npages;i++)if(child->elf->pages[i].flags&2){cb=child->elf->pages[i].virt;break;}
    CHECK(cb && cm_syscall(12,0,UINT64_MAX,1,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(2,0,cb,1,0)==CM_ERR_DENIED);
    CHECK(cm_syscall(12,1,cb,1,0)==CM_ERR_DENIED);
#ifdef CAPMIX_HOSTED
    CHECK(cm_syscall(12,0,cb,128,0)==(int64_t)sizeof(payload)-1);
    CHECK(cm_syscall(2,1,cb,sizeof(payload)-1,0)==(int64_t)sizeof(payload)-1);
    CHECK(cm_syscall(13,1,0,0,0)==CM_OK);
    CHECK(cm_syscall(2,1,cb,1,0)==CM_ERR_INVAL); /* no console fallback after close */
#endif
    cm_sched_set_current(NULL);
    for(unsigned step=0;step<512 && (!child->exited || (child->main && child->main->id));step++)
        (void)cm_sched_run_until_idle(1);
    cm_sched_set_current(p->main);CHECK(child->exited && (!child->main || !child->main->id));
    CHECK(cm_syscall(33,out.handle,base+320,1,0)==CM_OK);childpid=0;child=NULL;
    CHECK(cm_syscall(16,output,0,0,0)==CM_OK && cm_syscall(13,output,0,0,0)==CM_OK);
    char actual[sizeof(payload)]={0};CHECK(cm_vfs_open("/stdio-out.txt",CM_O_RDONLY,&f)==CM_OK);
    CHECK(cm_vfs_read(f,actual,sizeof(actual))==(int64_t)sizeof(payload)-1);
    CHECK(!memcmp(actual,payload,sizeof(payload)-1));cm_vfs_close(f);f=NULL;
    kinfo("stdio snapshot: files=%u/%u free+clean-cache=%llu/%llu",cm_vfs_open_files(),files_before,(unsigned long)(cm_mm_free_pages()+cm_file_cache_pages()),(unsigned long)stdio_before);
    CHECK(cm_vfs_open_files()==files_before && cm_mm_free_pages()+cm_file_cache_pages()==stdio_before);
    CHECK(cm_copyout(p->elf,base+128,"/bin/hello.elf",15)==CM_OK);
    kinfo("stdio redirect verified: copyin/rights denied parent-close child-read/write payload=ok fd-reclaim=no-leak");
    /* Explicit console delegation: no ambient keyboard authority, no management
     * authority in the recipient, and no allocation before rights validation. */
    uint64_t grant_before=cm_mm_free_pages();
    req.flags=CM_SPAWN_CONSOLE;
    CHECK(cm_copyout(p->elf,base,&req,sizeof(req))==CM_OK);
    cm_handle_t *parent_tty=cm_handle_lookup(p->task,2);CHECK(parent_tty);
    cm_rights_t saved_rights=parent_tty->rights;
    parent_tty->rights&=~CM_RIGHT_DUPLICATE;
    int64_t denied_grant=cm_syscall(3,1,base,base+64,0);
    parent_tty->rights=saved_rights;
    CHECK(denied_grant==CM_ERR_DENIED && cm_mm_free_pages()==grant_before);
    req.flags=CM_SPAWN_CONSOLE|CM_SPAWN_SHELL;
    CHECK(cm_copyout(p->elf,base,&req,sizeof(req))==CM_OK);
    CHECK(cm_syscall(3,1,base,base+64,0)==CM_ERR_INVAL);
    req.flags=CM_SPAWN_CONSOLE;
    CHECK(cm_copyout(p->elf,base,&req,sizeof(req))==CM_OK);
    CHECK(cm_syscall(3,1,base,base+64,0)==CM_OK);
    CHECK(cm_copyin(p->elf,&out,base+64,sizeof(out))==CM_OK);
    childpid=out.pid;child=cm_proc_lookup(childpid);CHECK(child && out.cpu==0);
    cm_handle_t *tty=cm_handle_lookup(child->task,1);
    CHECK(tty && tty->obj->type==CM_OBJ_CONSOLE && tty->rights==(CM_RIGHT_READ|CM_RIGHT_WRITE));
    cm_sched_set_current(child->main);
    CHECK(cm_syscall(32,1,0,0,0)==CM_OK);
    CHECK(cm_syscall(31,1,UINT64_MAX,1,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(37,3,0,0,0)==CM_ERR_DENIED);
    CHECK(cm_syscall(3,1,UINT64_MAX,UINT64_MAX,0)==CM_ERR_DENIED);
    cm_sched_set_current(NULL);
    for(unsigned step=0;step<512 && (!child->exited || (child->main && child->main->id));step++)
        (void)cm_sched_run_until_idle(1);
    cm_sched_set_current(p->main);
    CHECK(child->exited && (!child->main || !child->main->id));
    CHECK(cm_syscall(33,out.handle,base+320,1,0)==CM_OK);childpid=0;child=NULL;
    CHECK(cm_mm_free_pages()==grant_before);
    req.flags=0;
    kinfo("user console delegation verified: granted=READ/WRITE denied=no-duplicate no-control/boot reap=no-leak");
    /* Start a noncooperative ring3 loop, then request capability-scoped
     * termination. The target CPU must stop its context before resource reap. */
    CHECK(cm_copyout(p->elf,base+128,"/bin/spin.elf",14)==CM_OK);
    req.cpu=cm_cpu_count()>1?1:0;
    CHECK(cm_copyout(p->elf,base,&req,sizeof(req))==CM_OK);
    CHECK(cm_syscall(3,1,base,base+64,0)==CM_OK);
    CHECK(cm_copyin(p->elf,&out,base+64,sizeof(out))==CM_OK);
    childpid=out.pid;child=cm_proc_lookup(childpid);CHECK(child);
    cm_sched_set_current(NULL);
#ifndef CAPMIX_HOSTED
    cm_smp_sched_enable();
    uint64_t until=cm_rdtsc()+cm_tsc_hz()*2;
    while((!child->elf_started || !child->elf_preempts ||
           __atomic_load_n(&child->syscall_depth,__ATOMIC_ACQUIRE)) && cm_rdtsc()<until)
        (void)cm_sched_run_until_idle(1);
    CHECK(child->elf_started && child->elf_preempts && !__atomic_load_n(&child->syscall_depth,__ATOMIC_ACQUIRE));
#else
    for(unsigned step=0;step<64;step++)(void)cm_sched_run_until_idle(1);
#endif
    cm_sched_set_current(p->main);
    weak=cm_handle_install(p->task,cm_handle_lookup(p->task,out.handle)->obj,CM_RIGHT_WAIT);CHECK(weak>0);
    CHECK(cm_syscall(CM_SYS_CHILD_KILL,weak,0,0,0)==CM_ERR_DENIED);cm_handle_drop(p->task,weak);
    CHECK(cm_syscall(CM_SYS_CHILD_KILL,out.handle,1,0,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(CM_SYS_CHILD_KILL,out.handle,0,0,0)==CM_OK);
    cm_sched_set_current(NULL);
    uint64_t kill_until=cm_rdtsc()+cm_tsc_hz()*2;
    for(unsigned step=0;step<2000000 && (!__atomic_load_n(&child->exited,__ATOMIC_ACQUIRE) ||
        (child->main && __atomic_load_n(&child->main->id,__ATOMIC_ACQUIRE)));step++) {
        if(cm_rdtsc()>kill_until)break;
        (void)cm_sched_run_until_idle(1);
    }
    cm_smp_sched_disable();
    cm_sched_set_current(p->main);
    CHECK(child->exited && (!child->main || !child->main->id));
    CHECK(cm_syscall(33,out.handle,base+320,1,0)==CM_OK);childpid=0;child=NULL;
    CHECK(cm_copyin(p->elf,&result,base+320,sizeof(result))==CM_OK);
#ifndef CAPMIX_HOSTED
    CHECK(result.exit_code==137 && result.preempts>0);
    kinfo("user kill selftest: infinite ELF stopped exit=137 preempts=%llu reap=ok cap-denied=verified",(unsigned long)result.preempts);
#else
    CHECK(result.exit_code==CM_ERR_NOTSUP);
    kinfo("user kill selftest: hosted ring3 skipped cap-denied/request/reap verified");
#endif
    CHECK(cm_syscall(CM_SYS_CHILD_KILL,out.handle,0,0,0)==CM_ERR_DENIED);
    CHECK(cm_copyout(p->elf,base+128,"/bin/hello.elf",15)==CM_OK);req.cpu=0;
    CHECK(cm_copyout(p->elf,base,&req,sizeof(req))==CM_OK);
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
    cm_smp_sched_disable();
    cm_sched_set_current(NULL);
    if(f)cm_vfs_close(f);
    if(childpid && (child=cm_proc_lookup(childpid))) {
        /* A failing test must not recreate the remote live-stack free race. */
        if(!__atomic_load_n(&child->exited,__ATOMIC_ACQUIRE))
            (void)cm_proc_request_kill(child->parent_pid,childpid);
        cm_smp_sched_enable();
        uint64_t cleanup_until=cm_rdtsc()+cm_tsc_hz()*2;
        for(unsigned step=0;step<2000000 && cm_rdtsc()<cleanup_until;step++) {
            if(__atomic_load_n(&child->exited,__ATOMIC_ACQUIRE) &&
               (!child->main || !__atomic_load_n(&child->main->id,__ATOMIC_ACQUIRE)))break;
            (void)cm_sched_run_until_idle(1);
        }
        cm_smp_sched_disable();
        if(__atomic_load_n(&child->exited,__ATOMIC_ACQUIRE) &&
           (!child->main || !__atomic_load_n(&child->main->id,__ATOMIC_ACQUIRE)))
            (void)cm_proc_release_child(child->parent_pid,childpid);
        else {kerr("user control cleanup: live child retained, never force-free its context");failed++;}
    }
    if(p){cm_fdio_close_all(p);cm_control_process_exit(p);p->exited=1;p->main->state=CM_TH_DEAD;cm_thread_reap(p->main);(void)cm_proc_release_elf(pid);}
    if(cm_mm_free_pages()+cm_file_cache_pages()!=before){kerr("user control image/child-cap snapshot leak");failed++;}
    kinfo("user control selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
