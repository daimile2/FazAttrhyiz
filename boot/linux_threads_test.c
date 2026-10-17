#include "capmix/kernel.h"
#include "capmix/cap_internal.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/uaccess.h"
#include "capmix/sha256.h"
#include "../build/linux_threads.inc"
#include <string.h>
#define CHECK(x) do{if(!(x)){kerr("linux threads line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
static int fd_clean(cm_proc_t *p)
{
    if(p->linux_fd_owner || p->linux_fd_depth)return 0;
    for(unsigned i=0;i<CM_FD_MAX;i++)if(p->fd[i]>=0)return 0;
    uint64_t flags=cm_cap_lock();unsigned retained=0;
    for(int h=1;h<CM_HANDLES_PER_TASK;h++) {
        cm_handle_t *entry=cm_handle_lookup(p->task,h);
        if(entry && (entry->obj->type==CM_OBJ_FILE || entry->obj->type==CM_OBJ_LINUX_PIPE))retained++;
    }
    cm_cap_unlock(flags);
    if(retained)return 0;
    kinfo("linux thread fd cleanup verified: tgid=%llu fd-empty=1 file/pipe-handles=0 gate=idle",(unsigned long)p->pid);
    return 1;
}
static int run(const char *mode,int code,int cpu)
{
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),charge,limit,after;cm_commit_stats(&charge,&limit);
    const char *args[]={"/bin/linux-threads.elf",mode,NULL};
    cm_spawn_attr_t a={.path=args[0],.argv=args,.qos=CM_QOS_USER_INTERACTIVE,.cpu_set=1,.cpu=cpu};
    CHECK(cm_posix_spawn(&a,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
#ifdef CAPMIX_HOSTED
    cm_sched_set_current(p->main);
    CHECK(cm_linux_dispatch(202,UINT64_MAX,128,0,0)==-22);
    CHECK(cm_linux_dispatch(202,0x7fc000,128,1,0)==-11);
    CHECK(cm_linux_dispatch(202,0x7fc000,129,1,0)==0);
    CHECK(cm_linux_dispatch(202,0x7fc000,2,0,0)==-38);
    CHECK(cm_linux_dispatch(56,0,0,0,0)==-38);
    CHECK(cm_linux_dispatch(186,0,0,0,0)==(int64_t)pid);
    cm_linux_regs_t frame={0};p->linux_frame=&frame;
    int64_t socket=cm_linux_dispatch(41,2,1,0,0);CHECK(socket>=3);
    CHECK(cm_linux_clone(0x10f00,CM_ELF_STACK_TOP,0,0,0)==-95);
    CHECK(cm_linux_dispatch(3,(uint64_t)socket,0,0,0)==0);
    int64_t filefd=cm_native_open_path("/thread-fd-hosted.txt",CM_O_RDWR|CM_O_CREAT|CM_O_TRUNC);CHECK(filefd>=3);
    CHECK(cm_linux_dispatch(22,0x7fc100,0,0,0)==0);
    int pipes[2];CHECK(!cm_copyin(p->elf,pipes,0x7fc100,sizeof(pipes)));
    int64_t tid=cm_linux_clone(0x10f00,CM_ELF_STACK_TOP,0,0,0);CHECK(tid>0);
    cm_proc_t *t=cm_proc_lookup((cm_id_t)tid);CHECK(t && t->elf==p->elf && t->task==p->task && t->fd==p->fd && t->elf_entry_slot!=p->elf_entry_slot);
    t->linux_frame=&frame;cm_sched_set_current(t->main);
    CHECK(cm_linux_dispatch(39,0,0,0,0)==(int64_t)pid && cm_linux_dispatch(186,0,0,0,0)==tid);
    CHECK(cm_linux_dispatch(3,1,0,0,0)==-95);
    CHECK(cm_linux_dispatch(33,1,0,0,0)==-95);
    CHECK(cm_linux_dispatch(3,31,0,0,0)==-9);
    CHECK(cm_linux_dispatch(33,(uint64_t)pipes[0],10,0,0)==-95);
    CHECK(cm_linux_dispatch(33,(uint64_t)filefd,(uint64_t)pipes[1],0,0)==-95);
    CHECK(cm_linux_dispatch(3,(uint64_t)pipes[0],0,0,0)==-95);
    CHECK(cm_linux_dispatch(2,0,0,0,0)==-95 && cm_linux_dispatch(72,1,1,0,0)==-95);
    /* Deterministic suspended-owner model: no spin and no half mutation. */
    __atomic_store_n(&p->linux_fd_owner,p->pid,__ATOMIC_RELEASE);p->linux_fd_depth=1;
    CHECK(cm_linux_dispatch(33,(uint64_t)filefd,10,0,0)==-11 && p->fd[10]<0);
    CHECK(cm_linux_dispatch(1,1,0x7fc000,0,0)==-11);
    CHECK(p->linux_fd_owner==p->pid && p->linux_fd_depth==1);
    p->linux_fd_depth=0;__atomic_store_n(&p->linux_fd_owner,0,__ATOMIC_RELEASE);
    CHECK(cm_linux_dispatch(33,(uint64_t)filefd,10,0,0)==10 && p->fd[10]>=0);
    CHECK(cm_linux_dispatch(3,(uint64_t)filefd,0,0,0)==0);
    CHECK(cm_linux_dispatch(3,(uint64_t)filefd,0,0,0)==-9);
    CHECK(cm_linux_dispatch(33,1,10,0,0)==10 && p->linux_console[10]==p->linux_console[1]);
    CHECK(cm_linux_dispatch(3,10,0,0,0)==0 && p->fd[10]<0);
    int saved=p->fd[0];uint8_t console=p->linux_console[0];p->fd[0]=-1;p->linux_console[0]=0;
    CHECK(cm_linux_dispatch(32,1,0,0,0)==-95); /* never silently skip the lowest free fd */
    p->fd[0]=saved;p->linux_console[0]=console;
    CHECK(p->linux_fd_owner==0 && p->linux_fd_depth==0);
    kinfo("linux thread fd mechanism verified: shared FILE/console dup/close gate-contention=EAGAIN stdio/pipe/fd-hole rejected; gate released");
    CHECK(cm_linux_dispatch(57,0,0,0,0)==-95 && cm_linux_dispatch(59,0,0,0,0)==-95);
    CHECK(cm_linux_dispatch(9,0,4096,3,0x22)==-95);
    uint32_t marker=123,cleared=1;
    CHECK(cm_copyout(t->elf,0x7fc000,&marker,4)==0);
    CHECK(cm_linux_dispatch(218,0x7fc000,0,0,0)==tid);
    cm_proc_finish_exit(t,137);CHECK(!p->kill_pending && !p->linux_group_exit && t->exit_code==137);CHECK(cm_copyin(p->elf,&cleared,0x7fc000,4)==0 && !cleared);cm_sched_set_current(NULL);cm_thread_reap(t->main);p->linux_frame=NULL;
    cm_proc_finish_exit(p,0);cm_thread_reap(p->main);
    (void)code;
#else
    if(cpu)cm_smp_sched_enable();
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*8;
    while((!p->exited || (p->main && p->main->id==p->elf_thread_id)) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    kinfo("linux threads state mode=%s exited=%d code=%lld last=%llu ENOSYS=%llu",mode,p->exited,(long long)p->exit_code,(unsigned long)p->linux_last_syscall,(unsigned long)p->linux_enosys);
    CHECK(p->exited && p->exit_code==code && p->linux_enosys==2);
#endif
    CHECK(fd_clean(p));
    int release;
    uint64_t reap_end=cm_rdtsc()+cm_tsc_hz()*2;
    do {release=cm_proc_release_elf(pid);if(release==CM_ERR_BUSY)(void)cm_sched_run_until_idle(1);}while(release==CM_ERR_BUSY && cm_rdtsc()<reap_end);
    CHECK(release==0);pid=0;p=NULL;
    cm_commit_stats(&after,&limit);CHECK(charge==after && pages==cm_mm_free_pages()+cm_file_cache_pages());
    kinfo("linux threads verified: mode=%s exit=%d pages/commit/reap=ok",mode,code);
cleanup:
#ifdef CAPMIX_HOSTED
    cm_sched_set_current(NULL);
#endif
    if(p && !p->exited) {
        cm_linux_group_kill(p,137);
#ifndef CAPMIX_HOSTED
        uint64_t deadline=cm_rdtsc()+cm_tsc_hz()*3;
        while(!p->exited && cm_rdtsc()<deadline)(void)cm_sched_run_until_idle(1);
#else
        cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);
#endif
    }
    if(p && cm_proc_release_elf(pid))failed++;
    if(cpu)cm_smp_sched_disable();
    return failed;
}
#ifndef CAPMIX_HOSTED
static int pair(void)
{
    int failed=0;cm_id_t ids[2]={0};cm_proc_t *p[2]={0};
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),charge,limit,after;cm_commit_stats(&charge,&limit);
    cm_smp_sched_disable();
    for(unsigned i=0;i<2;i++) {
        const char *args[]={"/bin/linux-threads.elf","pair",NULL};
        cm_spawn_attr_t a={.path=args[0],.argv=args,.qos=CM_QOS_USER_INTERACTIVE,.cpu_set=1,.cpu=(int)i};
        CHECK(cm_posix_spawn(&a,&ids[i])==0);p[i]=cm_proc_lookup(ids[i]);CHECK(p[i]);
    }
    cm_smp_sched_enable();
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*8;
    for(unsigned i=0;i<2;i++) {
        while((!p[i]->exited || (p[i]->main && p[i]->main->id==p[i]->elf_thread_id)) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
        CHECK(p[i]->exited && p[i]->exit_code==0 && p[i]->linux_enosys==2);
        CHECK(fd_clean(p[i]));
    }
cleanup:
    for(unsigned i=0;i<2;i++)if(p[i]) {
        if(!p[i]->exited)cm_linux_group_kill(p[i],137);
        int e;uint64_t until=cm_rdtsc()+cm_tsc_hz()*3;
        do {e=cm_proc_release_elf(ids[i]);if(e)(void)cm_sched_run_until_idle(1);}while(e && cm_rdtsc()<until);
        if(e)failed++;
    }
    cm_smp_sched_disable();cm_commit_stats(&after,&limit);
    if(pages!=cm_mm_free_pages()+cm_file_cache_pages() || after!=charge)failed++;
    kinfo("linux futex group isolation: cpus=0/1 same-VA separate-mm TLS/WAIT/WAKE/reclaim %s",failed?"FAIL":"verified");
    return failed;
}
#endif
int cm_linux_threads_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("linux threads skipped: persona disabled");return 0;}
    uint8_t digest[32];char hex[65];const char digits[]="0123456789abcdef";
    if(cm_sha256(cm_linux_threads_elf,sizeof(cm_linux_threads_elf),digest))return 1;
    for(unsigned i=0;i<32;i++){hex[2*i]=digits[digest[i]>>4];hex[2*i+1]=digits[digest[i]&15];}hex[64]=0;
    kinfo("linux threads fixture: sha256=%s bytes=%llu",hex,(unsigned long)sizeof(cm_linux_threads_elf));
    cm_file_t *f=NULL;int e=cm_vfs_open("/bin/linux-threads.elf",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);
    if(e)return 1;
    e=cm_vfs_write(f,cm_linux_threads_elf,sizeof(cm_linux_threads_elf))==sizeof(cm_linux_threads_elf)?cm_vfs_fsync(f):CM_ERR_IO;
    cm_vfs_close(f);if(e)return 1;
    int failed=run("join",0,0);
#ifndef CAPMIX_HOSTED
    failed+=run("leader",0,0);failed+=run("group",9,0);
    if(cm_cpu_count()>1){failed+=run("ap",0,1);failed+=pair();}
#else
    kinfo("linux threads hosted: futex errno/key/wake mechanism verified; CPL3 clone/blocking skipped");
#endif
    kinfo("linux threads selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
