#include "capmix/linux_pipe.h"
#include "capmix/uaccess.h"
#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/busybox_sh.inc"
#define CHECK(x) do{if(!(x)){kerr("linux sh line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
/* Trusted mechanism test invokes Linux dispatch against a stopped image. */
static int pipes(void)
{
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;
    uint64_t before=cm_mm_free_pages()+cm_file_cache_pages();
    cm_spawn_attr_t attr={.path="/bin/busybox-sh.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    cm_sched_set_current(p->main);uint64_t scratch=0x7fc000;int fds[2];int64_t result;
    CHECK(cm_linux_dispatch(293,UINT64_MAX,0,0,0)==-14);
    CHECK(cm_linux_dispatch(293,scratch,1,0,0)==-22);
    CHECK(cm_linux_dispatch(293,scratch,0x80800,0,0)==0);
    CHECK(cm_copyin(p->elf,fds,scratch,8)==0 && cm_linux_pipe_live()==1);
    CHECK(cm_linux_dispatch(0,fds[0],scratch+256,4,0)==-11);
    CHECK(cm_copyout(p->elf,scratch+256,"pipe",4)==0);
    CHECK(cm_linux_dispatch(1,fds[1],scratch+256,4,0)==4);
    CHECK(cm_linux_dispatch(0,fds[1],scratch+256,4,0)==-9);
    CHECK(cm_linux_dispatch(1,fds[0],scratch+256,4,0)==-9);
    CHECK(cm_linux_dispatch(33,fds[1],10,0,0)==10);
    CHECK(cm_linux_dispatch(3,fds[1],0,0,0)==0);
    CHECK(cm_linux_dispatch(0,fds[0],UINT64_MAX,4,0)==-14);
    CHECK(cm_linux_dispatch(0,fds[0],scratch+512,4,0)==4);
    CHECK(!memcmp(cm_elf_pointer(p->elf,scratch+512),"pipe",4));
    CHECK(cm_linux_dispatch(3,10,0,0,0)==0);
    CHECK(cm_linux_dispatch(0,fds[0],scratch+256,4,0)==0);
    CHECK(cm_linux_dispatch(3,fds[0],0,0,0)==0 && !cm_linux_pipe_live());
    CHECK(cm_linux_dispatch(22,scratch,0,0,0)==0 && cm_copyin(p->elf,fds,scratch,8)==0);
    CHECK(cm_linux_dispatch(3,fds[0],0,0,0)==0);
    CHECK(cm_linux_dispatch(1,fds[1],scratch+256,4,0)==-32);
    CHECK(cm_linux_dispatch(3,fds[1],0,0,0)==0 && !cm_linux_pipe_live());
    for(unsigned i=0;i<8;i++)CHECK(cm_linux_dispatch(22,scratch,0,0,0)==0);
    CHECK(cm_linux_dispatch(22,scratch,0,0,0)==-23 && cm_linux_pipe_live()==8);
    for(unsigned i=3;i<CM_FD_MAX;i++)if(p->fd[i]>=0)CHECK(cm_linux_file_call(3,i,0,0,0,&result) && !result);
    CHECK(!cm_linux_pipe_live());
    kinfo("linux sh pipe mechanism: cap rights/dup/EOF/EPIPE/EAGAIN/EFAULT/capacity=8 cleanup verified");
cleanup:
    cm_sched_set_current(NULL);
    if(p){cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    if(cm_linux_pipe_live() || cm_mm_free_pages()+cm_file_cache_pages()!=before){kerr("linux sh pipe mechanism reclaim mismatch");failed++;}
    return failed;
}
#ifndef CAPMIX_HOSTED
static void close_output(void *data){(void)cm_vfs_close(data);}
static int command(const char *name,const char *script,const char *expected,int code)
{
    int failed=0;cm_file_t *output=NULL;cm_object_t *object=NULL;cm_proc_t *p=NULL;cm_id_t pid=0;
    unsigned files=cm_vfs_open_files();uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),commit,limit;
    cm_commit_stats(&commit,&limit);
    CHECK(cm_vfs_open("/sh-captured",CM_O_RDWR|CM_O_CREAT|CM_O_TRUNC,&output)==0);
    object=cm_obj_mint_owned(CM_OBJ_FILE,output,CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE,close_output);CHECK(object);output=NULL;
    const char *args[]={"/bin/busybox-sh.elf","sh","-c",script,NULL};
    cm_spawn_attr_t a={.path=args[0],.argv=args,.qos=CM_QOS_USER_INTERACTIVE,.stdio={NULL,object}};
    CHECK(cm_posix_spawn(&a,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*5;
    while((!p->exited || p->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    kinfo("linux sh state %s: exited=%d tid=%llu code=%lld last=%llu calls=%llu ENOSYS=%llu",name,p->exited,(unsigned long)p->main->id,(long long)p->exit_code,(unsigned long)p->linux_last_syscall,(unsigned long)p->linux_calls,(unsigned long)p->linux_enosys);
    if(!p->exited)kinfo("linux sh pending: rip=%llx preempts=%llu",(unsigned long)p->linux_regs.rcx,(unsigned long)p->elf_preempts);
    CHECK(p->exited && !p->main->id && p->exit_code==code && p->linux_enosys==0);
    static char bytes[8192];memset(bytes,0,sizeof(bytes));int64_t n=cm_vfs_pread(object->payload,bytes,sizeof(bytes)-1,0);
    if(n>=0)kinfo("linux sh captured %s: bytes=%lld",name,(long long)n);
    CHECK(n==(int64_t)strlen(expected) && !memcmp(bytes,expected,(size_t)n));
    CHECK(cm_vfs_fsync(object->payload)==0);
    kinfo("linux sh %s: CPL=3 output verified exit=%d",name,code);
cleanup:
    if(output)cm_vfs_close(output);
    if(object)cm_obj_release(object);
    if(pid && p){
        for(cm_id_t child=pid+1;child<pid+CM_MAX_PROCS;child++) {
            cm_proc_t *c=cm_proc_lookup(child);if(!c || c->parent_pid!=pid)continue;
            cm_proc_finish_exit(c,137);if(c->main && c->main->id)cm_thread_reap(c->main);
            if(cm_proc_release_child(pid,child))failed++;
        }
        if(!p->exited || p->main->id){cm_proc_finish_exit(p,137);if(p->main->id)cm_thread_reap(p->main);}if(cm_proc_release_elf(pid))failed++;}
    uint64_t after;cm_commit_stats(&after,&limit);
    if(cm_linux_pipe_live() || cm_vfs_open_files()!=files || cm_mm_free_pages()+cm_file_cache_pages()!=pages || after!=commit){kerr("linux sh reclaim mismatch: %s",name);failed++;}
    return failed;
}
#endif
int cm_linux_sh_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("linux sh skipped: persona disabled; native shell retained");return 0;}
    int failed=0;cm_file_t *f=NULL;static char large[6001];
    memset(large,'x',6000);large[6000]=0;
    CHECK(cm_vfs_open("/bin/busybox-sh.elf",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f)==0);
    CHECK(cm_vfs_write(f,cm_busybox_sh_elf,sizeof(cm_busybox_sh_elf))==sizeof(cm_busybox_sh_elf));
    CHECK(cm_vfs_fsync(f)==0);cm_vfs_close(f);f=NULL;
    CHECK(cm_vfs_open("/sh-captured",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f)==0);
    CHECK(cm_vfs_fsync(f)==0);cm_vfs_close(f);f=NULL;
    CHECK(cm_vfs_open("/sh-large.txt",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f)==0);
    CHECK(cm_vfs_write(f,large,6000)==6000 && cm_vfs_fsync(f)==0);cm_vfs_close(f);f=NULL;
    static uint64_t enosys_before[CM_LINUX_ENOSYS_BUCKETS];
    cm_linux_enosys_snapshot(enosys_before);
    failed+=pipes();
#ifndef CAPMIX_HOSTED
    failed+=command("basic","x=sh-basic-ok; echo $x; if true; then echo condition-ok; fi","sh-basic-ok\ncondition-ok\n",0);
    failed+=command("subshell","x=parent; (x=child; echo $x); echo $x","child\nparent\n",0);
    failed+=command("exec","/bin/busybox-sh.elf echo exec-ok; echo parent-ok","exec-ok\nparent-ok\n",0);
    failed+=command("pipe","echo pipe-ok | /bin/busybox-sh.elf cat","pipe-ok\n",0);
    failed+=command("stream","/bin/busybox-sh.elf cat /sh-large.txt | /bin/busybox-sh.elf cat",large,0);
    failed+=command("missing","/missing-command","",127);
    failed+=command("exit","exit 7","",7);
    cm_linux_enosys_dump("hush",enosys_before);
#endif
cleanup:
    if(f)cm_vfs_close(f);
    kinfo("linux sh selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
