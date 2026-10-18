#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/linux_uapi.h"
#include "capmix/fs.h"
#include "../build/linux_sigchld.inc"
#define CHECK(x) do{if(!(x)){kerr("SIGCHLD line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
int cm_linux_sigchld_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("SIGCHLD selftest skipped: persona disabled");return 0;}
    int failed=0;cm_proc_t *p=NULL,*child=NULL;cm_id_t pid=0,cid=0;
    uint64_t commit,limit;cm_commit_stats(&commit,&limit);
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages();
    cm_spawn_attr_t a={.path="/bin/linux-hello.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&a,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);cm_sched_set_current(p->main);
    uint64_t u=0x7fc000,bit=1ull<<16,pending=0;
    for(unsigned policy=0;policy<3;policy++) {
        cm_linux_sigaction_t act={.handler=policy==1,.flags=policy==2?2:0};
        CHECK(cm_copyout(p->elf,u,&act,sizeof(act))==0);
        CHECK(cm_linux_dispatch(13,17,u,u+64,8)==0);
        CHECK(cm_linux_dispatch(13,17,0,u+64,8)==0);
        cm_linux_sigaction_t old;CHECK(cm_copyin(p->elf,&old,u+64,sizeof(old))==0 && old.handler==act.handler && old.flags==act.flags);
        CHECK(cm_copyout(p->elf,u+128,&bit,8)==0 && cm_linux_dispatch(14,2,u+128,0,8)==0);
        CHECK(cm_posix_spawn(&a,&cid)==0);child=cm_proc_lookup(cid);CHECK(child);
        child->parent_pid=pid;child->linux_fork_child=1;
        CHECK(cm_linux_chld_policy(p,policy==1?0:1,&pending)==-16);
        cm_proc_finish_exit(child,42);if(child->main->id)cm_thread_reap(child->main);
        cm_linux_child_events();
        CHECK(cm_linux_dispatch(127,u+128,8,0,0)==0);
        CHECK(cm_copyin(p->elf,&pending,u+128,8)==0 && pending==(policy==1?0:bit));
        if(!policy){CHECK(cm_linux_wait4(cid,u+192,0,0)==(int64_t)cid);uint32_t st;CHECK(cm_copyin(p->elf,&st,u+192,4)==0 && st==(42<<8));}
        else CHECK(cm_linux_wait4(cid,u+192,0,0)==-10);
        CHECK(!cm_proc_lookup(cid));child=NULL;cid=0;
        CHECK(cm_copyout(p->elf,u+128,&(uint64_t){0},8)==0 && cm_linux_dispatch(14,2,u+128,0,8)==0);
        CHECK(!p->linux_sigpending);
    }
    kinfo("SIGCHLD mechanism: DFL waitable + blocked pending; IGN/SA_NOCLDWAIT ECHILD; off-CPU autoreap verified");
cleanup:
    cm_sched_set_current(NULL);
    if(child && cm_proc_lookup(cid)){cm_proc_finish_exit(child,137);if(child->main && child->main->id)cm_thread_reap(child->main);if(cm_proc_release_elf(cid))failed++;}
    if(p){cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    uint64_t after;cm_commit_stats(&after,&limit);
    if(after!=commit || cm_mm_free_pages()+cm_file_cache_pages()!=pages){kerr("SIGCHLD mechanism reclaim mismatch");failed++;}
#ifndef CAPMIX_HOSTED
    if(!failed) {
        cm_file_t *f=NULL;int e=cm_vfs_open("/bin/linux-sigchld.elf",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);
        if(e)failed++;
        else {if(cm_vfs_write(f,cm_linux_sigchld_elf,sizeof(cm_linux_sigchld_elf))!=(int64_t)sizeof(cm_linux_sigchld_elf))failed++;cm_vfs_close(f);}
        uint64_t guest_commit,guest_limit;cm_commit_stats(&guest_commit,&guest_limit);
        uint64_t guest_pages=cm_mm_free_pages()+cm_file_cache_pages();
        if(!failed && !cm_posix_spawn(&(cm_spawn_attr_t){.path="/bin/linux-sigchld.elf",.qos=CM_QOS_USER_INTERACTIVE},&pid)) {
            p=cm_proc_lookup(pid);uint64_t end=cm_rdtsc()+cm_tsc_hz()*5;
            while(p && (!p->exited || p->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
            if(!p || !p->exited || p->main->id || p->exit_code || p->linux_enosys!=1)failed++;
            if(p){if(!p->exited)cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
        } else failed++;
        cm_commit_stats(&after,&limit);
        if(after!=guest_commit || cm_mm_free_pages()+cm_file_cache_pages()!=guest_pages){kerr("SIGCHLD CPL3 reclaim mismatch");failed++;}
        kinfo("SIGCHLD CPL3 fork/wait/pending/auto-reap: %s; sigreturn ENOSYS counted=1",failed?"FAIL":"PASS");
    }
#else
    kinfo("SIGCHLD hosted: lifecycle mechanism verified; CPL3 fixture execution skipped");
#endif
    kinfo("SIGCHLD selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
