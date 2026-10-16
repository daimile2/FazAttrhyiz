#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/mm.h"
#include "capmix/proc.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include "capmix/cap_internal.h"
#include <string.h>
#include "../build/linux_l2.inc"
#include "../build/linux_l2_next.inc"
#define CHECK(x) do{if(!(x)){kerr("linux L2 line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
static int stage(const char *path,const void *data,uint64_t n)
{
    cm_file_t *f=NULL;int e=cm_vfs_open(path,CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);if(e)return e;
    if(cm_vfs_write(f,data,n)!=(int64_t)n)e=CM_ERR_IO;
    if(!e)e=cm_vfs_fsync(f);
    cm_vfs_close(f);return e;
}
static int mechanism(void)
{
    int failed=0;cm_elf_image_t *a=NULL,*b=NULL,*c=NULL,*denied=NULL;
    uint64_t committed,limit,after,held=0;cm_commit_stats(&committed,&limit);
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),split=cm_elf_cow_splits();
    uint64_t va=CM_ELF_STACK_TOP-CM_PAGE_SIZE,value=0x1234,other=0x5678;
    CHECK(cm_elf_load("/bin/linux-l2.elf",&a)==0);
    CHECK(cm_copyout(a,va,&value,8)==0);
    CHECK(cm_elf_clone_cow(a,&b)==0);
    CHECK(cm_elf_pointer(a,va)==cm_elf_pointer(b,va));
    CHECK(!(cm_space_lookup(a->root,va)&2) && !(cm_space_lookup(b->root,va)&2));
    CHECK(!(cm_space_lookup(a->shadow,va)&2) && !(cm_space_lookup(b->shadow,va)&2));
    CHECK(cm_copyout(b,va,&other,8)==0);
    CHECK(*(uint64_t *)cm_elf_pointer(a,va)==value && *(uint64_t *)cm_elf_pointer(b,va)==other);
    CHECK(cm_elf_pointer(a,va)!=cm_elf_pointer(b,va) && cm_elf_cow_splits()==split+1);
    uint64_t base=b->brk_end;
    CHECK(cm_elf_brk(b,base+8192)==base+8192 && *(uint64_t *)cm_elf_pointer(b,base)==0);
    CHECK(cm_elf_clone_cow(b,&c)==0);CHECK(cm_copyout(c,base,&value,8)==0);
    CHECK(*(uint64_t *)cm_elf_pointer(b,base)==0);
    cm_elf_destroy(b);b=NULL;CHECK(*(uint64_t *)cm_elf_pointer(c,va)==other);
    CHECK(cm_elf_brk(c,base)==base && !cm_elf_pointer(c,base));
    CHECK(cm_elf_brk(c,base+8192)==base+8192 && *(uint64_t *)cm_elf_pointer(c,base)==0);
    CHECK(cm_elf_brk(c,CM_ELF_STACK_TOP)==base+8192);
    CHECK(cm_elf_brk(c,base+(CM_LINUX_MAX_PAGES+1)*CM_PAGE_SIZE)==base+8192);
    CHECK(cm_commit_charge(UINT64_MAX)==CM_ERR_OVERCOMMIT);
    cm_commit_stats(&after,&limit);held=limit-after;
    CHECK(cm_commit_charge(held)==0);
    CHECK(cm_elf_clone_cow(a,&denied)==CM_ERR_OVERCOMMIT && !denied);
    cm_commit_uncharge(held);held=0;
    kinfo("linux L2 mechanism: RO shared -> copyout COW independent; nested refs/brk zero/shrink/limit/commit denial verified resident-only swap=excluded");
cleanup:
    if(held)cm_commit_uncharge(held);
    cm_elf_destroy(denied);cm_elf_destroy(a);cm_elf_destroy(b);cm_elf_destroy(c);
    cm_commit_stats(&after,&limit);
    if(after!=committed || cm_mm_free_pages()+cm_file_cache_pages()!=pages){kerr("linux L2 mechanism reclaim mismatch commit=%llu/%llu pages=%llu/%llu",(unsigned long)after,(unsigned long)committed,(unsigned long)(cm_mm_free_pages()+cm_file_cache_pages()),(unsigned long)pages);failed++;}
    return failed;
}
/* Fail after the COW image was cloned: a nonduplicable FILE must not be
 * smuggled into a child. Neither the task nor reserves/charge may leak. */
static int rollback(void)
{
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;cm_object_t *o=NULL;
    unsigned files=cm_vfs_open_files();uint64_t committed,limit;cm_commit_stats(&committed,&limit);
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages();
    cm_spawn_attr_t attr={.path="/bin/linux-l2.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    cm_sched_set_current(p->main);cm_linux_regs_t frame={0};p->linux_frame=&frame;
    const char path[]="/l2-file.txt";CHECK(cm_copyout(p->elf,0x7fc000,path,sizeof(path))==0);
    int64_t fd=cm_linux_dispatch(2,0x7fc000,0,0,0);CHECK(fd>=3);
    CHECK(cm_native_file_acquire((int)fd,CM_RIGHT_READ,&o)==0);
    int restricted=cm_handle_install(p->task,o,CM_RIGHT_READ);cm_obj_release(o);o=NULL;CHECK(restricted>0);
    CHECK(cm_native_close((uint64_t)fd)==0);p->fd[fd]=restricted;
    uint64_t charged,free_before=cm_mm_free_pages();cm_commit_stats(&charged,&limit);
    CHECK(cm_linux_fork()==-13);
    uint64_t after;cm_commit_stats(&after,&limit);
    CHECK(after==charged && cm_mm_free_pages()==free_before && !cm_proc_children_pending(pid));
    CHECK(cm_copyout(p->elf,0x7fc000,path,sizeof(path))==0); /* failed fork may leave safe RO singleton PTEs */
    kinfo("linux L2 fork denied FILE DUPLICATE: rollback=ok reserves/commit/task reclaimed parent writable");
cleanup:
    if(o)cm_obj_release(o);
    cm_sched_set_current(NULL);
    if(p){p->linux_frame=NULL;cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    uint64_t final;cm_commit_stats(&final,&limit);
    if(final!=committed || cm_vfs_open_files()!=files || cm_mm_free_pages()+cm_file_cache_pages()!=pages){kerr("linux L2 rollback reclaim mismatch");failed++;}
    return failed;
}
int cm_linux_l2_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("linux L2 skipped: persona disabled; native fork=NOTSUP");return 0;}
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;
    CHECK(stage("/bin/linux-l2.elf",cm_linux_l2_elf,sizeof(cm_linux_l2_elf))==0);
    CHECK(stage("/bin/linux-l2-next.elf",cm_linux_l2_next_elf,sizeof(cm_linux_l2_next_elf))==0);
    static const char sample[]="linux L2 file payload\n";
    CHECK(stage("/l2-file.txt",sample,sizeof(sample)-1)==0);
    failed+=mechanism();
    failed+=rollback();
#ifndef CAPMIX_HOSTED
    uint64_t before,limit;cm_commit_stats(&before,&limit);
    uint64_t free_before=cm_mm_free_pages()+cm_file_cache_pages(),splits=cm_elf_cow_splits();
    cm_spawn_attr_t attr={.path="/bin/linux-l2.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*5;
    while((!p->exited || p->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    CHECK(p->exited && !p->main->id && p->exit_code==0 && p->linux_enosys==0);
    CHECK(cm_elf_cow_splits()>splits);
    CHECK(cm_proc_release_elf(pid)==0);pid=0;p=NULL;
    uint64_t after;cm_commit_stats(&after,&limit);
    CHECK(after==before && cm_mm_free_pages()+cm_file_cache_pages()==free_before);
    kinfo("linux L2 CPL3 verified: fork parent/child independent, hardware write fault + copyout, exec same-pid argv/FS/CLOEXEC, wait4 status=42 reclaim=ok");
#else
    kinfo("linux L2 hosted: memory mechanism verified; CPL3 fork/exec execution skipped");
#endif
cleanup:
    if(p){cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    kinfo("linux L2 selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
