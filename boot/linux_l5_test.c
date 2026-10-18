#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/mm.h"
#include "capmix/proc.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/linux_l5.inc"
#include "../build/linux_l5_musl.inc"
#define CHECK(x) do{if(!(x)){kerr("linux L5 line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
static int file_mapping(void)
{
    int failed=0;cm_file_t *f=NULL;cm_elf_image_t *a=NULL,*b=NULL;
    uint64_t charge,limit,now,pages=cm_mm_free_pages()+cm_file_cache_pages();cm_commit_stats(&charge,&limit);
    CHECK(cm_elf_load("/bin/linux-mmap.elf",&a)==0);
    CHECK(cm_vfs_open("/data/mmap-private.bin",CM_O_RDONLY,&f)==0);
    CHECK(cm_elf_mmap_file(a,0,4096,3,2,f,0)==-95);
    CHECK(cm_elf_mmap_file(a,0,4096,1,1,f,0)==-95);
    CHECK(cm_elf_mmap_file(a,0,4096,1,2,f,1)==-22);
    CHECK(cm_elf_mmap_file(a,0,12288,1,2,f,0)==-22);
    CHECK(cm_elf_mmap_file(a,0,4096,1,2,f,8192)==-22);
    uint64_t before;cm_commit_stats(&before,&limit);
    int64_t va=cm_elf_mmap_file(a,0,8192,1,2,f,0);CHECK(va>=0);
    cm_commit_stats(&now,&limit);CHECK(now==before+2);
    CHECK(cm_elf_user_range(a,va,8192,4) && !cm_elf_user_range(a,va,1,2));
    CHECK(*(char *)cm_elf_pointer(a,va)=='A' && *(char *)cm_elf_pointer(a,va+4096)=='B');
    CHECK(!*(char *)cm_elf_pointer(a,va+5000) && !*(char *)cm_elf_pointer(a,va+8191));
    char x='X';CHECK(cm_copyout(a,va,&x,1)!=0);
    uint64_t held=limit-now;CHECK(cm_commit_charge(held)==0);
    int64_t rejected=cm_elf_mmap_file(a,0,4096,1,2,f,0);cm_commit_uncharge(held);CHECK(rejected==-12);
    CHECK(cm_vfs_seek(f,0,1)==0);
    int64_t off=cm_elf_mmap_file(a,0,904,1,2,f,4096);CHECK(off>=0);
    CHECK(*(char *)cm_elf_pointer(a,off)=='B' && !*(char *)cm_elf_pointer(a,off+904));
    CHECK(cm_elf_munmap(a,off,4096)==0);
    /* Only writable anonymous/image pages reserve a fork copy, not file RO pages. */
    unsigned writable=0;for(unsigned i=0;i<a->npages;i++)writable+=!!(cm_elf_page_at(a,i)->flags&2);
    cm_commit_stats(&before,&limit);CHECK(cm_elf_clone_cow(a,&b)==0);
    cm_commit_stats(&now,&limit);CHECK(now==before+writable);
    CHECK(cm_elf_pointer(a,va)==cm_elf_pointer(b,va));
    cm_vfs_close(f);f=NULL;
    CHECK(cm_elf_munmap(a,va,8192)==0);
    CHECK(*(char *)cm_elf_pointer(b,va+4096)=='B');
    /* Even the dynamic protection helper cannot upgrade file snapshots. */
    b->rtld=1;CHECK(cm_elf_mprotect(b,va,8192,3)==-95);b->rtld=0;
    CHECK(cm_elf_munmap(b,va,8192)==0);
    kinfo("linux file mmap mechanism: readonly snapshot offset/EOF/zero-tail fd-close fork-share write/shared denied commit-limit verified");
cleanup:
    if(f)cm_vfs_close(f);
    cm_elf_destroy(a);cm_elf_destroy(b);
    cm_commit_stats(&now,&limit);
    if(now!=charge || cm_mm_free_pages()+cm_file_cache_pages()!=pages){kerr("linux file mmap reclaim mismatch");failed++;}
    if(!failed)kinfo("linux file mmap reclaim: pages/commit baseline restored");
    return failed;
}
static int mechanism(void)
{
    int failed=0;cm_elf_image_t *a=NULL,*b=NULL,*c=NULL,*denied=NULL;
    uint64_t initial,limit,now,held=0,back,res,baseback,baseres;void *physical_held=NULL;
    cm_commit_stats(&initial,&limit);cm_elf_cow_stats(&baseback,&baseres);
    uint64_t free=cm_mm_free_pages()+cm_file_cache_pages();
    CHECK(cm_elf_load("/bin/linux-mmap.elf",&a)==0);
    unsigned image=a->npages;
    int64_t top=cm_elf_mmap(a,CM_LINUX_USER_MAX-4096,4096,3,0x22,-1,0);
    CHECK(top==CM_LINUX_USER_MAX-4096 && cm_elf_user_range(a,top,4096,6));
    CHECK(cm_elf_munmap(a,top,4096)==0 && !cm_elf_pointer(a,top));
    CHECK(cm_elf_mmap(a,0,4096,3,0x21,-1,0)==-95);
    CHECK(cm_elf_mmap(a,0,4096,3,2,0,0)==-95);
    CHECK(cm_elf_mmap(a,0,0,3,0x22,-1,0)==-22);
    CHECK(cm_elf_mmap(a,0,UINT64_MAX,3,0x22,-1,0)==-22);
    int64_t va=cm_elf_mmap(a,0,1024*1024,3,0x22,-1,0);CHECK(va==CM_LINUX_MMAP_MIN);
    CHECK(a->npages==image+256 && cm_elf_user_range(a,va,1024*1024,6));
    for(unsigned i=0;i<256;i++){uint64_t *q=cm_elf_pointer(a,va+i*4096);CHECK(q && !*q);*q=i+1;}
    unsigned writable=0;for(unsigned i=0;i<a->npages;i++)writable+=!!(cm_elf_page_at(a,i)->flags&2);
    uint64_t before;cm_commit_stats(&before,&limit);cm_elf_cow_stats(&back,&res);uint64_t oldback=back,oldres=res;
    CHECK(cm_elf_clone_cow(a,&b)==0);cm_commit_stats(&now,&limit);cm_elf_cow_stats(&back,&res);
    CHECK(now==before+writable && back==oldback && res==oldres+writable);
    uint64_t value=0x12345678;CHECK(cm_copyout(b,va,&value,8)==0);
    CHECK(*(uint64_t *)cm_elf_pointer(a,va)==1 && *(uint64_t *)cm_elf_pointer(b,va)==value);
    cm_commit_stats(&now,&limit);cm_elf_cow_stats(&back,&res);
    CHECK(now==before+writable && back==oldback+1 && res==oldres+writable-1);
    kinfo("linux L5 commit stats: before=%llu fork=%llu writable=%u reserve=%llu split_charge_unchanged=1",
          (unsigned long)before,(unsigned long)now,writable,(unsigned long)res);
    CHECK(cm_elf_clone_cow(b,&c)==0);value=0xabcdef;CHECK(cm_copyout(c,va+4096,&value,8)==0);
    CHECK(*(uint64_t *)cm_elf_pointer(b,va+4096)==2);
    cm_elf_destroy(a);a=NULL;cm_elf_destroy(b);b=NULL;
    CHECK(*(uint64_t *)cm_elf_pointer(c,va)==0x12345678);
    CHECK(cm_elf_munmap(c,va+4096,8192)==0 && !cm_elf_pointer(c,va+4096));
    CHECK(cm_elf_mmap(c,va+4096,8192,3,0x22,-1,0)==va+4096);
    CHECK(*(uint64_t *)cm_elf_pointer(c,va+4096)==0);
    CHECK(cm_elf_munmap(c,va,1024*1024)==0 && c->npages==image);
    CHECK(cm_elf_munmap(c,va,1024*1024)==0); /* holes are permitted */
    CHECK(cm_elf_munmap(c,0x400000,4096)==-22);
    uint64_t n=CM_LINUX_MAX_PAGES-c->npages;
    va=cm_elf_mmap(c,0,n*4096,3,0x22,-1,0);CHECK(va>=0);
    CHECK(c->npages==CM_LINUX_MAX_PAGES);
    for(uint64_t i=0;i<n;i++){uint8_t *q=cm_elf_pointer(c,va+i*4096);CHECK(q && !*q);*q=(uint8_t)i;}
    CHECK(cm_elf_mmap(c,0,4096,3,0x22,-1,0)==-12);
    CHECK(cm_elf_munmap(c,va,n*4096)==0 && c->npages==image);
    cm_commit_stats(&before,&limit);held=limit-before;CHECK(cm_commit_charge(held)==0);
    uint64_t available=cm_mm_free_pages();CHECK(cm_elf_mmap(c,0,4096,3,0x22,-1,0)==-12);
    CHECK(cm_elf_clone_cow(c,&denied)==CM_ERR_OVERCOMMIT && !denied);
    CHECK(cm_mm_free_pages()==available);cm_commit_uncharge(held);held=0;
    /* Force a mid-mapping physical failure after some data/PT allocation.
     * Held pages are a local intrusive list: no allocator changes/test hooks. */
    uint64_t fill=(CM_ELF_MAX_PAGES-1-c->npages)*4096;
    int64_t filled=cm_elf_mmap(c,0,fill,3,0x22,-1,0);CHECK(filled>=0);
    uint64_t free_before=cm_mm_free_pages();cm_commit_stats(&before,&limit);
    while(cm_mm_free_pages()>8){void *q=cm_page_alloc(0);CHECK(q);*(void **)q=physical_held;physical_held=q;}
    CHECK(cm_elf_mmap(c,0x1200000,16*4096,3,0x22,-1,0)==-12);
    CHECK(cm_mm_free_pages()==8 && c->npages==CM_ELF_MAX_PAGES-1 && !c->extra[0]);
    cm_elf_cow_stats(&oldback,&oldres);
    CHECK(cm_elf_clone_cow(c,&denied)==CM_ERR_NOMEM && !denied);
    cm_elf_cow_stats(&back,&res);CHECK(back==oldback && res==oldres && cm_mm_free_pages()==8);
    cm_commit_stats(&now,&limit);CHECK(now==before);
    while(physical_held){void *q=physical_held;physical_held=*(void **)q;cm_page_free(q,0);}
    CHECK(cm_mm_free_pages()==free_before);
    CHECK(cm_elf_munmap(c,filled,fill)==0 && c->npages==image);
    kinfo("linux L5 partial allocation failure: data/PT/descriptor/commit rollback=ok");
    kinfo("linux L5 mmap: window=16MiB quota=8MiB touched=2048-image pages zero/limit/partial-unmap verified");
    kinfo("linux L5 COW commit: reserve charged, split charge unchanged, nested parent-first + denial rollback verified resident-only swap=excluded");
cleanup:
    while(physical_held){void *q=physical_held;physical_held=*(void **)q;cm_page_free(q,0);}
    if(held)cm_commit_uncharge(held);
    cm_elf_destroy(a);cm_elf_destroy(b);cm_elf_destroy(c);cm_elf_destroy(denied);
    cm_commit_stats(&now,&limit);cm_elf_cow_stats(&back,&res);
    if(now!=initial || back!=baseback || res!=baseres || cm_mm_free_pages()+cm_file_cache_pages()!=free){kerr("linux L5 reclaim: commit=%llu/%llu backing=%llu/%llu reserve=%llu/%llu pages=%llu/%llu",(unsigned long)now,(unsigned long)initial,(unsigned long)back,(unsigned long)baseback,(unsigned long)res,(unsigned long)baseres,(unsigned long)(cm_mm_free_pages()+cm_file_cache_pages()),(unsigned long)free);failed++;}
    if(!failed)kinfo("linux L5 reclaim: pages/commit/backings/reserves baseline restored");
    return failed;
}
int cm_linux_l5_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("linux L5 skipped: persona disabled; native ABI retained");return 0;}
    int failed=0;cm_file_t *f=NULL;cm_proc_t *p=NULL;cm_id_t pid=0;
    CHECK(cm_vfs_open("/bin/linux-mmap.elf",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f)==0);
    CHECK(cm_vfs_write(f,cm_linux_l5_elf,sizeof(cm_linux_l5_elf))==sizeof(cm_linux_l5_elf));
    CHECK(cm_vfs_fsync(f)==0);cm_vfs_close(f);f=NULL;
    CHECK(cm_vfs_open("/bin/musl-mmap.elf",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f)==0);
    CHECK(cm_vfs_write(f,cm_linux_l5_musl,sizeof(cm_linux_l5_musl))==sizeof(cm_linux_l5_musl));
    CHECK(cm_vfs_fsync(f)==0);cm_vfs_close(f);f=NULL;
    CHECK(cm_vfs_mkdir("/data")==0 || cm_vfs_stat("/data",&(cm_stat_t){0})==0);
    CHECK(cm_vfs_open("/data/mmap-private.bin",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f)==0);
    char buffer[1000];memset(buffer,'A',sizeof(buffer));
    for(unsigned i=0;i<4;i++)CHECK(cm_vfs_write(f,buffer,1000)==1000);
    CHECK(cm_vfs_write(f,buffer,96)==96);memset(buffer,'B',sizeof(buffer));
    CHECK(cm_vfs_write(f,buffer,904)==904);CHECK(cm_vfs_fsync(f)==0);cm_vfs_close(f);f=NULL;
    failed+=file_mapping();
    failed+=mechanism();
#ifndef CAPMIX_HOSTED
    for(unsigned test=0;test<2;test++) {
    cm_spawn_attr_t attr={.path=test?"/bin/musl-mmap.elf":"/bin/linux-mmap.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*5;
    while((!p->exited || p->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    kinfo("linux L5 CPL3 image=%s exited=%d code=%d ENOSYS=%llu last=%u",attr.path,p->exited,p->exit_code,(unsigned long)p->linux_enosys,p->linux_last_syscall);
    CHECK(p->exited && !p->main->id && p->exit_code==0 && p->linux_enosys==0);
    CHECK(cm_proc_release_elf(pid)==0);p=NULL;
    }
    kinfo("linux L5 CPL3 mmap/fork/munmap + musl allocator verified: exit=0 ENOSYS=0");
#endif
cleanup:
    if(f)cm_vfs_close(f);
    if(p){cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    kinfo("linux L5 selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
