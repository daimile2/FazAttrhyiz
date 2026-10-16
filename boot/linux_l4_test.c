#include "capmix/kernel.h"
#include "capmix/boot.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/linux_l4_main.inc"
#include "../build/linux_l4_interp.inc"
#include "../build/linux_l4_exec.inc"
#define MAIN "/bin/linux-l4a.elf"
#define EXEC "/bin/linux-l4a-exec.elf"
#define BAD "/bin/linux-l4a-bad.elf"
#define CHECK(x) do{if(!(x)){kerr("linux L4a line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
static int stage(const char *path,const void *bytes,uint64_t n)
{
    cm_file_t *f=NULL;int e=cm_vfs_open(path,CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);if(e)return e;
    if(cm_vfs_write(f,bytes,n)!=(int64_t)n)e=CM_ERR_IO;
    if(!e)e=cm_vfs_fsync(f);
    cm_vfs_close(f);return e;
}
static uint64_t available(void){return cm_mm_free_pages()+cm_file_cache_pages();}
/* Each rejected image must release a partially built main/interpreter and all commit. */
static int rejected(const char *path,int expected)
{
    uint64_t before=available(),used,limit,after;cm_commit_stats(&used,&limit);
    cm_elf_image_t *im=NULL;int e=cm_elf_load(path,&im),bad=e!=expected || im!=NULL;
    cm_elf_destroy(im);cm_commit_stats(&after,&limit);
    if(bad || before!=available() || used!=after){kerr("linux L4a rejection e=%d expected=%d pages=%llu/%llu commit=%llu/%llu",e,expected,(unsigned long)before,(unsigned long)available(),(unsigned long)used,(unsigned long)after);return 1;}
    return 0;
}
static int mechanism(void)
{
    int failed=0;cm_elf_image_t *im=NULL,*child=NULL;void *mutant=NULL;uint64_t held=0,used,limit,after;
    uint64_t before=available();cm_commit_stats(&used,&limit);
    CHECK(cm_elf_load(MAIN,&im)==0);
    CHECK(im->abi==CM_ABI_LINUX && im->interp_base==CM_ELF_INTERP_BASE && im->entry!=im->main_entry);
    CHECK(im->entry>=CM_ELF_INTERP_BASE && im->main_entry<CM_ELF_INTERP_BASE && im->phdr==0x400040 && im->phnum==3);
    CHECK(im->charged==im->npages && cm_elf_user_range(im,im->entry,1,5) && cm_elf_user_range(im,im->main_entry,1,5));
    uint64_t stack[20];CHECK(cm_copyin(im,stack,im->stack,sizeof(stack))==0);
    CHECK(stack[0]==1 && !stack[2] && !stack[3]);
    unsigned mask=0;
    for(unsigned i=4;i+1<20 && stack[i];i+=2){
        if(stack[i]==9){CHECK(stack[i+1]==im->main_entry);mask|=1;}
        if(stack[i]==7){CHECK(stack[i+1]==im->interp_base);mask|=2;}
        if(stack[i]==3){CHECK(stack[i+1]==im->phdr);mask|=4;}
    }
    CHECK(mask==7);
    CHECK(cm_elf_brk(im,CM_ELF_INTERP_BASE+1)==im->brk_base);
    CHECK(cm_elf_clone_cow(im,&child)==0 && child->interp_base==im->interp_base && child->main_entry==im->main_entry);
    uint64_t value=0x12345678;CHECK(cm_copyout(child,0x401000,&value,8)==0);
    CHECK(*(uint64_t *)cm_elf_pointer(im,0x401000)==0);
    cm_elf_destroy(child);child=NULL;cm_elf_destroy(im);im=NULL;
    CHECK(before==available());cm_commit_stats(&after,&limit);CHECK(after==used);
    mutant=cm_page_alloc(2);CHECK(mutant && sizeof(cm_linux_l4_main)<4*CM_PAGE_SIZE && sizeof(cm_linux_l4_interp)<4*CM_PAGE_SIZE);
    cm_elf64_ehdr_t *eh=mutant;cm_elf64_phdr_t *ph;
    for(unsigned test=0;test<6;test++) {
        memcpy(mutant,cm_linux_l4_main,sizeof(cm_linux_l4_main));ph=(void *)((char *)mutant+eh->phoff);
        int expected=CM_ERR_INVAL;
        if(test==0)ph[2].filesz=1;
        if(test==1)ph[1]=ph[2]; /* second PT_INTERP */
        if(test==2){eh->ident[7]=0;expected=CM_ERR_NOTSUP;} /* never change native persona */
        if(test==3){ph[0].vaddr=CM_ELF_INTERP_BASE;eh->entry+=CM_ELF_INTERP_BASE-CM_ELF_USER_MIN;}
        if(test==4){((char *)mutant)[ph[2].offset]='x';expected=CM_ERR_NOTSUP;}
        if(test==5){ph[1].memsz=CM_LINUX_MAX_PAGES*CM_PAGE_SIZE;expected=CM_ERR_INVAL;} /* exceeds the fixed L4a main/interpreter address partition */
        CHECK(stage(BAD,mutant,sizeof(cm_linux_l4_main))==0 && !rejected(BAD,expected));
    }
    for(unsigned test=0;test<6;test++) {
        memcpy(mutant,cm_linux_l4_interp,sizeof(cm_linux_l4_interp));ph=(void *)((char *)mutant+eh->phoff);
        int expected=CM_ERR_INVAL;
        if(test==0)eh->type=2;
        if(test==1){ph[0].type=3;expected=CM_ERR_NOTSUP;} /* recursive interpreter */
        if(test==2){ph[0].type=7;expected=CM_ERR_NOTSUP;} /* TLS */
        if(test==3){ph[0].type=2;expected=CM_ERR_NOTSUP;} /* dynamic deps/relocations */
        if(test==4)eh->entry=CM_ELF_INTERP_END-CM_ELF_INTERP_BASE;
        if(test==5){eh->phnum=2;ph[1]=ph[0];} /* overlapping PT_LOAD */
        CHECK(stage(CM_ELF_INTERP_PATH,mutant,sizeof(cm_linux_l4_interp))==0 && !rejected(MAIN,expected));
    }
    CHECK(stage(CM_ELF_INTERP_PATH,cm_linux_l4_interp,sizeof(cm_linux_l4_interp))==0);
    /* Allow the 6-page main+stack charge, deny the extra interpreter page. */
    cm_commit_stats(&after,&limit);CHECK(limit>after+6);held=limit-after-6;
    CHECK(cm_commit_charge(held)==0);CHECK(!rejected(MAIN,CM_ERR_OVERCOMMIT));cm_commit_uncharge(held);held=0;
    CHECK(cm_vfs_unlink(CM_ELF_INTERP_PATH)==0 && !rejected(MAIN,CM_ERR_NOENT));
    cm_spawn_attr_t attr={.path=MAIN,.qos=CM_QOS_USER_INTERACTIVE};cm_id_t untouched=77;
    uint64_t spawn_before=available();
    CHECK(cm_posix_spawn(&attr,&untouched)==CM_ERR_NOENT && untouched==77 && spawn_before==available());
    CHECK(stage(CM_ELF_INTERP_PATH,cm_linux_l4_interp,sizeof(cm_linux_l4_interp))==0);
    kinfo("linux L4a mechanism verified: auxv/main-entry/AT_BASE COW brk-boundary; malformed/overlap/capacity/commit/missing rollback=ok");
cleanup:
    if(held)cm_commit_uncharge(held);
    if(mutant)cm_page_free(mutant,2);
    cm_elf_destroy(child);cm_elf_destroy(im);
    cm_commit_stats(&after,&limit);
    if(used!=after || before!=available()){kerr("linux L4a mechanism pages/commit reclaim mismatch");failed++;}
    /* Restore fixture even on a negative-test failure. */
    if(stage(CM_ELF_INTERP_PATH,cm_linux_l4_interp,sizeof(cm_linux_l4_interp)))failed++;
    return failed;
}
#ifndef CAPMIX_HOSTED
static int execute(const char *path,int missing)
{
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;
    uint64_t before=available(),used,limit,after;cm_commit_stats(&used,&limit);
    const char *argv[]={path,"spawn",NULL};cm_spawn_attr_t attr={.path=path,.argv=argv,.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*5;
    while((!p->exited || p->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    CHECK(p->exited && !p->main->id && p->exit_code==(missing?42:0) && p->linux_enosys==0);
    if(!missing){
        CHECK(p->elf->interp_base==CM_ELF_INTERP_BASE && !memcmp(p->elf->path,MAIN,sizeof(MAIN)));
        CHECK(*(uint64_t *)cm_elf_pointer(p->elf,0x401000)==1 && *(uint64_t *)cm_elf_pointer(p->elf,0x401008)==2);
        CHECK(p->linux_write_bytes==sizeof("linux L4a interp entered: CPL3 auxv verified\n")+sizeof("linux L4a main entered: hello via interpreter\n")-2);
    }
    CHECK(cm_proc_release_elf(pid)==0);p=NULL;
    cm_commit_stats(&after,&limit);CHECK(used==after && before==available());
    kinfo("linux L4a CPL3 %s verified: exit=%d ENOSYS=0 pages/commit reclaimed",missing?"exec-failure-old-image":path,missing?42:0);
cleanup:
    if(p){cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    return failed;
}
#endif
int cm_linux_l4_selftest(void)
{
    int failed=0;cm_elf_image_t *im=NULL;cm_stat_t st;
    int e=cm_vfs_stat("/lib",&st);CHECK(!e || (e==CM_ERR_NOENT && cm_vfs_mkdir("/lib")==0));
    CHECK(stage(MAIN,cm_linux_l4_main,sizeof(cm_linux_l4_main))==0);
    CHECK(stage(EXEC,cm_linux_l4_exec,sizeof(cm_linux_l4_exec))==0);
    CHECK(stage(CM_ELF_INTERP_PATH,cm_linux_l4_interp,sizeof(cm_linux_l4_interp))==0);
    if(!cm_linux_interp_enabled()) {
        CHECK(!rejected(MAIN,CM_ERR_NOTSUP));
        kinfo("linux L4a gate-off verified: PT_INTERP rejected native/static retained; Linux=%d",cm_linux_enabled());goto cleanup;
    }
    failed+=mechanism();if(failed)goto cleanup;
#ifndef CAPMIX_HOSTED
    failed+=execute(MAIN,0);failed+=execute(EXEC,0);
    CHECK(cm_vfs_unlink(CM_ELF_INTERP_PATH)==0);failed+=execute(EXEC,1);
    CHECK(stage(CM_ELF_INTERP_PATH,cm_linux_l4_interp,sizeof(cm_linux_l4_interp))==0);
#else
    kinfo("linux L4a hosted: image/auxv/rollback verified; CPL3 interpreter handoff skipped");
#endif
cleanup:
    cm_elf_destroy(im);kinfo("linux L4a selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
