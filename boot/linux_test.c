#include "capmix/kernel.h"
#include "capmix/abi.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/linux_hello.inc"
#include "../build/linux_probe.inc"
#define CHECK(x) do{if(!(x)){kerr("linux L0 line %d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
static cm_error_t stage(const char *path,const void *data,uint64_t size)
{
    cm_file_t *f=NULL;cm_error_t e=cm_vfs_open(path,CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f);if(e)return e;
    if(cm_vfs_write(f,data,size)!=(int64_t)size)e=CM_ERR_IO;
    if(!e)e=cm_vfs_fsync(f);
    cm_vfs_close(f);return e;
}
static int done(cm_proc_t *p){return p && p->exited && (!p->main || !p->main->id);}
static int wait_all(cm_proc_t **p,unsigned count)
{
#ifndef CAPMIX_HOSTED
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*5;
#endif
    for(unsigned i=0;i<1000000;i++) {
        unsigned n=0;for(unsigned j=0;j<count;j++)n+=done(p[j]);if(n==count)return 1;
#ifndef CAPMIX_HOSTED
        if(cm_rdtsc()>=end)break;
#endif
        (void)cm_sched_run_until_idle(1);
    }
    return 0;
}
int cm_linux_selftest(void)
{
    int failed=0;cm_elf_image_t *im=NULL;cm_proc_t *p[2]={0};cm_id_t pid[2]={0};
    void *mutant=NULL;uint64_t before=0;
    CHECK(stage("/bin/linux-hello.elf",cm_linux_hello_elf,sizeof(cm_linux_hello_elf))==CM_OK);
    CHECK(stage("/bin/linux-probe.elf",cm_linux_probe_elf,sizeof(cm_linux_probe_elf))==CM_OK);
    before=cm_mm_free_pages()+cm_file_cache_pages();
    mutant=cm_page_alloc(1);CHECK(mutant && sizeof(cm_linux_hello_elf)+64<8192);
    memcpy(mutant,cm_linux_hello_elf,sizeof(cm_linux_hello_elf));
    cm_elf64_ehdr_t *eh=mutant;cm_elf64_phdr_t *ph=(void *)((char *)mutant+eh->phoff);
    /* ELFOSABI_SYSV without a note remains native, independent of its filename. */
    eh->ident[7]=0;
    CHECK(stage("/bin/linux-marker-test.elf",mutant,sizeof(cm_linux_hello_elf))==CM_OK);
    CHECK(cm_elf_load("/bin/linux-marker-test.elf",&im)==CM_OK && im->abi==CM_ABI_CAPMIX);
    cm_elf_destroy(im);im=NULL;
    /* Bounded GNU/Linux ABI note; use otherwise-unneeded data PHDR as PT_NOTE. */
    uint32_t note[]={4,16,1,0x00554e47,0,2,6,0};
    memcpy((char *)mutant+sizeof(cm_linux_hello_elf),note,sizeof(note));
    ph[1].type=4;ph[1].offset=sizeof(cm_linux_hello_elf);ph[1].filesz=sizeof(note);
    CHECK(stage("/bin/linux-marker-test.elf",mutant,sizeof(cm_linux_hello_elf)+sizeof(note))==CM_OK);
    cm_error_t e=cm_elf_load("/bin/linux-marker-test.elf",&im);
    CHECK(e==(cm_linux_enabled()?CM_OK:CM_ERR_NOTSUP));
    if(im){CHECK(im->abi==CM_ABI_LINUX);cm_elf_destroy(im);im=NULL;}
    /* Conflicting explicit native marker must never choose the other syscall ABI. */
    uint32_t native_note[]={7,4,1,0x4d706143,0x00007869,0}; /* CapMix\0 + pad */
    memcpy((char *)mutant+sizeof(cm_linux_hello_elf)+sizeof(note),native_note,sizeof(native_note));
    ph[1].filesz+=sizeof(native_note);
    CHECK(stage("/bin/linux-marker-test.elf",mutant,sizeof(cm_linux_hello_elf)+sizeof(note)+sizeof(native_note))==CM_OK);
    CHECK(cm_elf_load("/bin/linux-marker-test.elf",&im)==CM_ERR_INVAL && !im);
    /* Truncated/oversized notes are rejected before any image publication. */
    ph[1].filesz=4097;
    CHECK(stage("/bin/linux-marker-test.elf",mutant,sizeof(cm_linux_hello_elf)+sizeof(note)+sizeof(native_note))==CM_OK);
    CHECK(cm_elf_load("/bin/linux-marker-test.elf",&im)==CM_ERR_INVAL && !im);
    ph[1].type=3;
    CHECK(stage("/bin/linux-marker-test.elf",mutant,sizeof(cm_linux_hello_elf)+sizeof(note)+sizeof(native_note))==CM_OK);
    CHECK(cm_elf_load("/bin/linux-marker-test.elf",&im)==CM_ERR_NOTSUP && !im);
    eh->type=3; /* usual dynamic Linux executable: still log PT_INTERP explicitly */
    CHECK(stage("/bin/linux-marker-test.elf",mutant,sizeof(cm_linux_hello_elf)+sizeof(note)+sizeof(native_note))==CM_OK);
    CHECK(cm_elf_load("/bin/linux-marker-test.elf",&im)==CM_ERR_NOTSUP && !im);
    memcpy(mutant,cm_linux_hello_elf,sizeof(cm_linux_hello_elf));
    ph=(void *)((char *)mutant+eh->phoff);ph[0].offset=4096; /* PHDR no longer mapped */
    CHECK(stage("/bin/linux-marker-test.elf",mutant,sizeof(cm_linux_hello_elf))==CM_OK);
    CHECK(cm_elf_load("/bin/linux-marker-test.elf",&im)==CM_ERR_NOTSUP && !im);
    cm_page_free(mutant,1);mutant=NULL;
    before=cm_mm_free_pages()+cm_file_cache_pages(); /* fixtures/cache intentionally persist */
    if(!cm_linux_enabled()) {
        CHECK(cm_elf_load("/bin/linux-hello.elf",&im)==CM_ERR_NOTSUP && !im);
        cm_spawn_attr_t a={.path="/bin/linux-hello.elf",.qos=CM_QOS_DEFAULT};cm_id_t unchanged=77;
        CHECK(cm_posix_spawn(&a,&unchanged)==CM_ERR_NOTSUP && unchanged==77);
        kinfo("linux L0 disabled verified: Linux spawn rejected; unmarked native load retained; PT_INTERP refused");goto cleanup;
    }
    CHECK(cm_elf_load("/bin/linux-hello.elf",&im)==CM_OK && im->abi==CM_ABI_LINUX);
    uint64_t words[24];CHECK(cm_copyin(im,words,im->stack,sizeof(words))==CM_OK);
    CHECK(words[0]==1 && words[2]==0 && words[3]==0);
    CHECK(words[4]==6 && words[5]==4096 && words[6]==3 && words[7]==im->phdr &&
          words[8]==4 && words[9]==56 && words[10]==5 && words[11]==2 &&
          words[12]==9 && words[13]==im->entry && words[14]==25 && words[16]==0 && words[17]==0);
    uint8_t random[16];CHECK(cm_copyin(im,random,words[15],16)==CM_OK);
    CHECK(cm_elf_user_range(im,im->phdr,im->phnum*im->phent,4));
    cm_elf_destroy(im);im=NULL;
    cm_spawn_attr_t a={.path="/bin/linux-hello.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&a,&pid[0])==CM_OK);p[0]=cm_proc_lookup(pid[0]);CHECK(p[0] && p[0]->abi==CM_ABI_LINUX);
#ifdef CAPMIX_HOSTED
    cm_sched_set_current(p[0]->main);
    CHECK(cm_syscall(9999,0,0,0,0)==-38 && cm_syscall(0x40000000,0,0,0,0)==-38);
    CHECK(cm_syscall(1,1,UINT64_MAX,1,0)==-14 && cm_syscall(1,100,0,0,0)==-9);
    CHECK(cm_syscall(158,0x1002,0xffff800000000000ull,0,0)==-1);
    CHECK(cm_syscall(158,0x1003,0x400000,0,0)==-14);
    CHECK(cm_syscall(158,0x1002,0x401000,0,0)==0);
    CHECK(cm_syscall(158,0x1003,0x401000,0,0)==0);
    CHECK(*(uint64_t *)cm_elf_pointer(p[0]->elf,0x401000)==0x401000);
    cm_arch_user_fs_set(0);cm_sched_set_current(NULL);
#endif
    CHECK(wait_all(p,1));
#ifndef CAPMIX_HOSTED
    CHECK(p[0]->exit_code==0 && p[0]->linux_write_bytes==sizeof("hello from Linux ABI L0\n")-1 && !p[0]->linux_enosys);
    kinfo("linux golden verified: CPL=3 write_bytes=%llu exit=0 ENOSYS=0",(unsigned long)p[0]->linux_write_bytes);
#else
    CHECK(p[0]->exit_code==CM_ERR_NOTSUP && p[0]->linux_enosys==2);
    kinfo("linux hosted: persona/auxv/copy/errno/ENOSYS/FS model verified; hardware execution skipped");
#endif
    CHECK(cm_proc_release_elf(pid[0])==CM_OK);pid[0]=0;p[0]=NULL;
    for(unsigned i=0;i<2;i++) {
        const char *args[]={"/bin/linux-probe.elf",i?"B":"A",NULL};
        a.path="/bin/linux-probe.elf";a.argv=args;
        CHECK(cm_posix_spawn(&a,&pid[i])==CM_OK);p[i]=cm_proc_lookup(pid[i]);CHECK(p[i]);
        *(uint64_t *)cm_elf_pointer(p[i]->elf,0x401008)=cm_tsc_hz()/5;
    }
    CHECK(wait_all(p,2));
#ifndef CAPMIX_HOSTED
    for(unsigned i=0;i<2;i++)CHECK(p[i]->exit_code==0 && p[i]->linux_enosys==2 && p[i]->elf_preempts &&
                                 *(uint64_t *)cm_elf_pointer(p[i]->elf,0x401000)==1);
    CHECK(CM_MIN(p[0]->elf_end_tsc,p[1]->elf_end_tsc)>CM_MAX(p[0]->elf_start_tsc,p[1]->elf_start_tsc));
    CHECK(cm_arch_user_fs_get()==0);
    kinfo("linux FS/auxv verified: same-cpu overlap preempts=%llu/%llu FS isolated scheduler_FS=0 ENOSYS=2/2",
          (unsigned long)p[0]->elf_preempts,(unsigned long)p[1]->elf_preempts);
#endif
cleanup:
#ifdef CAPMIX_HOSTED
    cm_sched_set_current(NULL);
#endif
    if(mutant)cm_page_free(mutant,1);
    cm_elf_destroy(im);
    for(unsigned i=0;i<2;i++)if(pid[i] && p[i]) {
        if(!done(p[i])) {
            cm_proc_finish_exit(p[i],137);
            if(p[i]->main && p[i]->main->id)cm_thread_reap(p[i]->main);
        }
        if(cm_proc_release_elf(pid[i])!=CM_OK)failed++;
    }
    if(before && cm_mm_free_pages()+cm_file_cache_pages()!=before){kerr("linux L0 image/page-table reclaim mismatch");failed++;}
    kinfo("linux L0 selftest: %s (%d failures) enabled=%u native ABI retained",failed?"FAIL":"PASS",failed,cm_linux_enabled());
    return failed;
}
