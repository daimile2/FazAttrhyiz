#include "capmix/kernel.h"
#include "capmix/abi.h"
#include "capmix/linux_uapi.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/busybox_l1.inc"
#define CHECK(x) do{if(!(x)){kerr("linux L1 line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
#define SCRATCH 0x7fc000ull
static int equal(const char *a,const char *b){size_t n=strlen(a);return n==strlen(b) && !memcmp(a,b,n);}
static const char payload[]="linux L1 cat payload\n";
static int stage(const char *path,const void *data,uint64_t n)
{
    cm_file_t *f=NULL;int e=cm_vfs_open(path,CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f);if(e)return e;
    if(cm_vfs_write(f,data,n)!=(int64_t)n)e=CM_ERR_IO;
    if(!e)e=cm_vfs_fsync(f);
    cm_vfs_close(f);return e;
}
static int put(cm_proc_t *p,uint64_t where,const char *s){return cm_copyout(p->elf,where,s,strlen(s)+1);}
static int descriptor_test(void)
{
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;unsigned files=cm_vfs_open_files();
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages();
    cm_spawn_attr_t a={.path="/bin/busybox-l1.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&a,&pid)==CM_OK);p=cm_proc_lookup(pid);CHECK(p && p->abi==CM_ABI_LINUX);
    cm_sched_set_current(p->main);
    CHECK(cm_syscall(2,UINT64_MAX,0,0,0)==-14);
    CHECK(!put(p,SCRATCH,"/l1-fixtures/sample.txt"));
    CHECK(cm_syscall(2,SCRATCH,0x40000000,0,0)==-22);
    CHECK(cm_syscall(2,SCRATCH,0x10000,0,0)==-20);
    CHECK(cm_syscall(2,SCRATCH,0xc1,0,0)==-17);
    int64_t fd=cm_syscall(2,SCRATCH,0,0,0);CHECK(fd>=3);
    CHECK(cm_syscall(0,(uint64_t)fd,0x400000,5,0)==-14);
    CHECK(cm_syscall(0,(uint64_t)fd,SCRATCH+256,5,0)==5);
    CHECK(!memcmp(cm_elf_pointer(p->elf,SCRATCH+256),payload,5));
    CHECK(cm_syscall(8,(uint64_t)fd,0,1,0)==5);
    CHECK(cm_syscall(33,(uint64_t)fd,10,0,0)==10);
    CHECK(cm_syscall(3,(uint64_t)fd,0,0,0)==0);
    CHECK(cm_syscall(0,10,SCRATCH+256,5,0)==5);
    CHECK(!memcmp(cm_elf_pointer(p->elf,SCRATCH+256),payload+5,5));
    CHECK(cm_syscall(8,10,0,1,0)==10);
    CHECK(cm_syscall(5,10,SCRATCH+512,0,0)==0);
    cm_linux_stat_t st;CHECK(!cm_copyin(p->elf,&st,SCRATCH+512,sizeof(st)));
    CHECK(st.size==sizeof(payload)-1 && (st.mode&0170000)==0100000 && st.blksize==4096 && st.unused[0]==0);
    CHECK(cm_syscall(33,1,20,0,0)==20); /* duplicate console independently of fd1 */
    CHECK(cm_syscall(33,10,1,0,0)==1);
    CHECK(cm_syscall(1,1,SCRATCH,1,0)==-13); /* read-only FILE capability */
    CHECK(cm_syscall(3,10,0,0,0)==0 && cm_syscall(3,1,0,0,0)==0);
    CHECK(!put(p,SCRATCH,"/l1-unit-output"));
    CHECK(cm_syscall(2,SCRATCH,0x241,0644,0)==1); /* closed stdout reused */
    CHECK(!put(p,SCRATCH+256,"fd-write"));
    CHECK(cm_syscall(1,1,SCRATCH+256,8,0)==8);
    CHECK(cm_syscall(33,1,10,0,0)==10 && cm_syscall(3,1,0,0,0)==0);
    CHECK(cm_syscall(0,10,SCRATCH,1,0)==-9);
    CHECK(cm_syscall(1,10,SCRATCH+256,8,0)==8);
    CHECK(cm_syscall(8,10,0,1,0)==16);
    CHECK(!put(p,SCRATCH+256,"linux L1 console alias preserved\n"));
    CHECK(cm_syscall(1,20,SCRATCH+256,33,0)==33);
    CHECK(cm_syscall(3,20,0,0,0)==0);
    CHECK(!put(p,SCRATCH,"/l1-fixtures"));
    fd=cm_syscall(257,(uint64_t)(int64_t)-100,SCRATCH,0x90000,0);CHECK(fd>=0);
    CHECK(cm_syscall(72,(uint64_t)fd,1,0,0)==1);
    CHECK(cm_syscall(217,(uint64_t)fd,SCRATCH+768,23,0)==-22);
    CHECK(cm_syscall(8,(uint64_t)fd,0,1,0)==0);
    int64_t bytes=cm_syscall(217,(uint64_t)fd,SCRATCH+768,256,0);CHECK(bytes>0);
    unsigned seen=0;uint64_t at=0;
    while(at<(uint64_t)bytes) {
        cm_linux_dirent64_t *d=cm_elf_pointer(p->elf,SCRATCH+768+at);
        CHECK(d->reclen>=24 && !(d->reclen&7) && at+d->reclen<=(uint64_t)bytes);
        if(equal(d->name,"sample.txt")){CHECK(d->type==8);seen++;}
        at+=d->reclen;
    }
    CHECK(seen==1 && cm_syscall(217,(uint64_t)fd,SCRATCH+768,256,0)==0);
    CHECK(!put(p,SCRATCH,"sample.txt"));
    int64_t relative=cm_syscall(257,(uint64_t)fd,SCRATCH,0,0);CHECK(relative>=0);
    CHECK(cm_syscall(0,(uint64_t)relative,SCRATCH+256,sizeof(payload)-1,0)==sizeof(payload)-1);
    CHECK(!memcmp(cm_elf_pointer(p->elf,SCRATCH+256),payload,sizeof(payload)-1));
    CHECK(cm_syscall(79,SCRATCH,2,0,0)==2 && equal(cm_elf_pointer(p->elf,SCRATCH),"/"));
    CHECK(cm_syscall(63,SCRATCH+1024,0,0,0)==0);
    cm_linux_utsname_t *u=cm_elf_pointer(p->elf,SCRATCH+1024);CHECK(equal(u->sysname,"CapMix") && equal(u->machine,"x86_64"));
    CHECK(cm_syscall(39,0,0,0,0)==(int64_t)pid);
    CHECK(cm_syscall(9,0,0,0,0)==-95 && cm_syscall(0x40000001,0,0,0,0)==-38 && p->linux_enosys==1);
    kinfo("linux L1 fd verified: copy/errno/stat144/dirent64/openat/seek dup-shared-offset close/console-alias mmap=EOPNOTSUPP unknown=ENOSYS");
cleanup:
    cm_sched_set_current(NULL);
    if(p){cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    if(cm_vfs_open_files()!=files || cm_mm_free_pages()+cm_file_cache_pages()!=pages){kerr("linux L1 fd reclaim mismatch");failed++;}
    return failed;
}
#ifndef CAPMIX_HOSTED
static void close_output(void *data){(void)cm_vfs_close(data);}
static int applet(const char *name,const char *arg,const char *expected)
{
    int failed=0;cm_file_t *output=NULL;cm_object_t *object=NULL;cm_proc_t *p=NULL;cm_id_t pid=0;
    unsigned files=cm_vfs_open_files();uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages();
    CHECK(cm_vfs_open("/l1-captured",CM_O_RDWR|CM_O_CREAT|CM_O_TRUNC,&output)==CM_OK);
    object=cm_obj_mint_owned(CM_OBJ_FILE,output,CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE,close_output);CHECK(object);output=NULL;
    const char *args[]={"/bin/busybox-l1.elf",name,arg,NULL};
    cm_spawn_attr_t a={.path=args[0],.argv=args,.qos=CM_QOS_USER_INTERACTIVE,.stdio={NULL,object}};
    CHECK(cm_posix_spawn(&a,&pid)==CM_OK);p=cm_proc_lookup(pid);CHECK(p);
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*5;
    while((!p->exited || p->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    kinfo("linux L1 applet state %s: exited=%d tid=%llu code=%lld last=%llu calls=%llu preempts=%llu",name,p->exited,(unsigned long)p->main->id,(long long)p->exit_code,(unsigned long)p->linux_last_syscall,(unsigned long)p->linux_calls,(unsigned long)p->elf_preempts);
    CHECK(p->exited && !p->main->id && p->exit_code==0 && p->linux_enosys==0);
    char bytes[256]={0};int64_t n=cm_vfs_pread(object->payload,bytes,sizeof(bytes),0);
    CHECK(n==(int64_t)strlen(expected) && !memcmp(bytes,expected,(size_t)n));
    CHECK(cm_vfs_fsync(object->payload)==CM_OK);
    kinfo("linux L1 busybox %s: CPL=3 output bytes=%lld verified exit=0 ENOSYS=0 BSS-heap=65536",name,(long long)n);
cleanup:
    if(output)cm_vfs_close(output);
    if(object)cm_obj_release(object);
    if(pid && p){if(!p->exited || p->main->id){cm_proc_finish_exit(p,137);if(p->main->id)cm_thread_reap(p->main);}if(cm_proc_release_elf(pid))failed++;}
    if(cm_vfs_open_files()!=files || cm_mm_free_pages()+cm_file_cache_pages()!=pages){kerr("linux L1 applet reclaim mismatch");failed++;}
    return failed;
}
#endif
int cm_linux_l1_selftest(void)
{
    int failed=0;
    if(!cm_linux_enabled()){kinfo("linux L1 skipped: persona disabled; native files retained");return 0;}
    CHECK(stage("/bin/busybox-l1.elf",cm_busybox_l1_elf,sizeof(cm_busybox_l1_elf))==CM_OK);
    cm_stat_t st;if(cm_vfs_stat("/l1-fixtures",&st))CHECK(cm_vfs_mkdir("/l1-fixtures")==CM_OK);
    CHECK(stage("/l1-fixtures/sample.txt",payload,sizeof(payload)-1)==CM_OK);
    CHECK(stage("/l1-unit-output","",0)==CM_OK && stage("/l1-captured","",0)==CM_OK);
    failed+=descriptor_test();
#ifndef CAPMIX_HOSTED
    failed+=applet("echo","linux L1 echo payload","linux L1 echo payload\n");
    failed+=applet("cat","/l1-fixtures/sample.txt",payload);
    failed+=applet("ls","/l1-fixtures","sample.txt\n");
#else
    kinfo("linux L1 hosted: fd/layout semantics verified; musl ELF execution skipped");
#endif
cleanup:
    kinfo("linux L1 selftest: %s (%d failures), custom musl BusyBox subset bounded BSS heap=65536",failed?"FAIL":"PASS",failed);
    return failed;
}
