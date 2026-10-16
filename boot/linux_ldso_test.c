#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/uaccess.h"
#include <string.h>
#include "../build/ldso_main.inc"
#include "../build/ldso_interp.inc"
#include "../build/ldso_lib.inc"
#define MAIN "/bin/linux-dynamic.elf"
#define LIB "/lib/libcmgold.so.1"
#define CHECK(x) do{if(!(x)){kerr("linux ldso line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
static int stage(const char *path,const void *bytes,uint64_t n)
{
    cm_file_t *f=NULL;int e=cm_vfs_open(path,CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);if(e)return e;
    if(cm_vfs_write(f,bytes,n)!=(int64_t)n)e=CM_ERR_IO;
    if(!e)e=cm_vfs_fsync(f);
    cm_vfs_close(f);return e;
}
static int rejected(const char *path,int expected)
{
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),before,limit,after;cm_commit_stats(&before,&limit);
    cm_elf_image_t *im=NULL;int e=cm_elf_load(path,&im),bad=e!=expected || im!=NULL;
    cm_elf_destroy(im);cm_commit_stats(&after,&limit);
    if(bad || before!=after || pages!=cm_mm_free_pages()+cm_file_cache_pages()){kerr("linux ldso reject rollback e=%d expected=%d",e,expected);return 1;}
    return 0;
}
#ifndef CAPMIX_HOSTED
static void close_output(void *data){(void)cm_vfs_close(data);}
static int execute(const char *label,const char *expected,int code)
{
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;cm_file_t *file=NULL;cm_object_t *output=NULL;
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),before,limit,after;cm_commit_stats(&before,&limit);unsigned files=cm_vfs_open_files();
    CHECK(cm_vfs_open("/ldso-captured",CM_O_CREAT|CM_O_TRUNC|CM_O_RDWR,&file)==0);
    output=cm_obj_mint_owned(CM_OBJ_FILE,file,CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE,close_output);CHECK(output);file=NULL;
    cm_spawn_attr_t attr={.path=MAIN,.qos=CM_QOS_USER_INTERACTIVE,.stdio={NULL,output}};
    CHECK(cm_posix_spawn(&attr,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*3;
    while((!p->exited || p->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    kinfo("linux ldso state %s: exited=%d code=%lld ENOSYS=%llu last=%llu",label,p->exited,(long long)p->exit_code,(unsigned long)p->linux_enosys,(unsigned long)p->linux_last_syscall);
    CHECK(p->exited && !p->main->id && p->exit_code==code && p->linux_enosys==0);
    char text[512]={0};int64_t n=cm_vfs_pread(output->payload,text,sizeof(text)-1,0);
    if(n>0)kinfo("linux ldso captured: %s",text);
    CHECK(n==(int64_t)strlen(expected) && !memcmp(text,expected,(size_t)n));
    CHECK(cm_vfs_fsync(output->payload)==0);
cleanup:
    if(file)cm_vfs_close(file);
    if(output)cm_obj_release(output);
    if(p){if(!p->exited || p->main->id){cm_proc_finish_exit(p,137);if(p->main->id)cm_thread_reap(p->main);}if(cm_proc_release_elf(pid))failed++;}
    cm_commit_stats(&after,&limit);
    if(before!=after || pages!=cm_mm_free_pages()+cm_file_cache_pages() || files!=cm_vfs_open_files()){kerr("linux ldso reclaim mismatch: %s",label);failed++;}
    if(!failed)kinfo("linux ldso %s: CPL3 exit=%d output verified ENOSYS=0 rollback/reclaim=ok",label,code);
    return failed;
}
#endif
int cm_linux_ldso_selftest(void)
{
    int failed=0;cm_elf_image_t *im=NULL;void *mutant=NULL;
    CHECK(stage(MAIN,cm_ldso_main,sizeof(cm_ldso_main))==0);
    if(!cm_linux_interp_enabled()) {
        failed+=rejected(MAIN,CM_ERR_NOTSUP);
        kinfo("linux ldso gate-off: dynamic main rejected; native/static retained");goto cleanup;
    }
    cm_stat_t st;if(cm_vfs_stat("/lib",&st))CHECK(cm_vfs_mkdir("/lib")==0);
    CHECK(stage(CM_ELF_LDSO_PATH,cm_ldso_interp,sizeof(cm_ldso_interp))==0);
    CHECK(stage(LIB,cm_ldso_lib,sizeof(cm_ldso_lib))==0);
    CHECK(stage("/ldso-captured","",0)==0);
    CHECK(cm_elf_load(MAIN,&im)==0);
    CHECK(im->rtld && im->abi==CM_ABI_LINUX && im->interp_base==CM_ELF_INTERP_BASE && im->entry!=im->main_entry);
    cm_elf_destroy(im);im=NULL;
    CHECK(sizeof(cm_ldso_main)<=8*CM_PAGE_SIZE && sizeof(cm_ldso_lib)<=8*CM_PAGE_SIZE);
    mutant=cm_page_alloc(3);CHECK(mutant);
    memcpy(mutant,cm_ldso_main,sizeof(cm_ldso_main));
    cm_elf64_ehdr_t *eh=mutant;cm_elf64_phdr_t *ph=(void *)((char *)mutant+eh->phoff);
    unsigned changed=0;
    for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==2){ph[i].offset=UINT64_MAX;changed=1;break;}
    CHECK(changed && stage("/bin/ldso-bad.elf",mutant,sizeof(cm_ldso_main))==0);
    failed+=rejected("/bin/ldso-bad.elf",CM_ERR_INVAL);
    memcpy(mutant,cm_ldso_main,sizeof(cm_ldso_main));
    changed=0;for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==0x6474e551u){ph[i].type=7;changed=1;break;}
    CHECK(changed && stage("/bin/ldso-bad.elf",mutant,sizeof(cm_ldso_main))==0);
    failed+=rejected("/bin/ldso-bad.elf",CM_ERR_NOTSUP);
    CHECK(cm_vfs_unlink(CM_ELF_LDSO_PATH)==0);
    failed+=rejected(MAIN,CM_ERR_NOENT);
    CHECK(stage(CM_ELF_LDSO_PATH,cm_ldso_interp,sizeof(cm_ldso_interp))==0);
    kinfo("linux ldso kernel validation: malformed PT_DYNAMIC/TLS/missing interpreter rollback=ok");
#ifndef CAPMIX_HOSTED
    failed+=execute("golden","ldso: loaded /lib/libcmgold.so.1\nldso: reloc RELATIVE=1 GLOB_DAT=4 JUMP_SLOT=1 R64=1\nlinux ldso: relocation verified\n",0);
    CHECK(cm_vfs_unlink(LIB)==0);
    failed+=execute("missing-library","ldso: reject library open\n",126);
    CHECK(stage(LIB,cm_ldso_lib,sizeof(cm_ldso_lib))==0);
    /* Fixed fixture DT_RELA -> file offset, no section-header dependency. */
    memcpy(mutant,cm_ldso_lib,sizeof(cm_ldso_lib));eh=mutant;ph=(void *)((char *)mutant+eh->phoff);
    uint64_t rela=0,offset=0;
    for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==2) {
        uint64_t *d=(void *)((char *)mutant+ph[i].offset);
        for(unsigned j=0;j<ph[i].filesz/16;j++)if(d[j*2]==7){rela=d[j*2+1];break;}
    }
    CHECK(rela);
    for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==1 && rela>=ph[i].vaddr && rela-ph[i].vaddr<ph[i].filesz)offset=ph[i].offset+rela-ph[i].vaddr;
    CHECK(offset && offset+24<=sizeof(cm_ldso_lib));
    ((uint64_t *)((char *)mutant+offset))[1]=37; /* R_X86_64_IRELATIVE unsupported */
    CHECK(stage(LIB,mutant,sizeof(cm_ldso_lib))==0);
    failed+=execute("unsupported-reloc","ldso: loaded /lib/libcmgold.so.1\nldso: unsupported relocation=37\nldso: reject relocation unsupported\n",126);
    memcpy(mutant,cm_ldso_lib,sizeof(cm_ldso_lib));
    ((uint64_t *)((char *)mutant+offset))[0]=0; /* write into R-only ELF header */
    CHECK(stage(LIB,mutant,sizeof(cm_ldso_lib))==0);
    failed+=execute("text-reloc","ldso: loaded /lib/libcmgold.so.1\nldso: reject relocation target (no text relocations)\n",126);
    memcpy(mutant,cm_ldso_lib,sizeof(cm_ldso_lib));
    changed=0;
    for(unsigned i=0;i+11<sizeof(cm_ldso_lib);i++)if(!memcmp((char *)mutant+i,"gold_value",11)){memcpy((char *)mutant+i,"gone_value",10);changed=1;}
    CHECK(changed && stage(LIB,mutant,sizeof(cm_ldso_lib))==0);
    failed+=execute("unresolved","ldso: loaded /lib/libcmgold.so.1\nldso: unresolved symbol gold_value\nldso: reject unresolved symbol\n",126);
    memcpy(mutant,cm_ldso_lib,sizeof(cm_ldso_lib));changed=0;
    for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==0x6474e551u){ph[i].type=7;changed=1;break;}
    CHECK(changed && stage(LIB,mutant,sizeof(cm_ldso_lib))==0);
    failed+=execute("TLS-library","ldso: reject TLS unsupported\n",126);
    CHECK(stage(LIB,cm_ldso_lib,sizeof(cm_ldso_lib))==0);
    failed+=execute("restored","ldso: loaded /lib/libcmgold.so.1\nldso: reloc RELATIVE=1 GLOB_DAT=4 JUMP_SLOT=1 R64=1\nlinux ldso: relocation verified\n",0);
#endif
cleanup:
    if(mutant){cm_page_free(mutant,3);if(stage(LIB,cm_ldso_lib,sizeof(cm_ldso_lib)) || stage(CM_ELF_LDSO_PATH,cm_ldso_interp,sizeof(cm_ldso_interp)))failed++;}
    cm_elf_destroy(im);
    kinfo("linux ldso selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
