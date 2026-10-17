#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/uaccess.h"
#include <string.h>
#include "../build/ldso_main.inc"
#include "../build/ldso_interp.inc"
#include "../build/ldso_lib.inc"
#include "../build/tls_main.inc"
#include "../build/tls_a.inc"
#include "../build/tls_b.inc"
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
/* Permission/COW mechanism tests never inject an uncontained CPL3 #PF. */
static int wx_mechanism(void)
{
    int failed=0;cm_elf_image_t *im=NULL,*child=NULL;
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),before,limit,after;
    cm_commit_stats(&before,&limit);
    CHECK(!cm_elf_load(MAIN,&im));
    uint64_t nx=1ull<<63;
    CHECK(!(cm_space_lookup(im->root,im->main_entry)&(nx|2)));
    CHECK((cm_space_lookup(im->root,CM_ELF_STACK_TOP-4096)&(nx|2))==(nx|2));
    int64_t a=cm_elf_mmap(im,0,3*4096,3,0x22,UINT64_MAX,0);CHECK(a>0);
    CHECK(cm_elf_mprotect(im,a,4096,7)==-1);
    CHECK(cm_elf_mprotect(im,a+1,4096,1)==-22);
    CHECK(cm_elf_mprotect(im,a,4096,0)==-95);
    CHECK(cm_elf_mprotect(im,a,4*4096,1)==-12);
    CHECK((cm_space_lookup(im->root,a)&(nx|2))==(nx|2)); /* failed range was atomic */
    CHECK(!cm_elf_mprotect(im,a,4096,1));
    uint64_t value=123;CHECK(cm_copyout(im,a,&value,8)==CM_ERR_INVAL);
    CHECK(!cm_elf_mprotect(im,a+4096,4096,5));
    CHECK(!(cm_space_lookup(im->root,a+4096)&(nx|2)));
    CHECK(!cm_elf_clone_cow(im,&child));
    CHECK(cm_elf_mprotect(im,a,4096,3)==-95); /* shared RO cannot gain write */
    CHECK(cm_elf_mprotect(im,a+8192,4096,1)==-95);
    CHECK(!cm_elf_cow_write(im,a+8192));
    CHECK((cm_space_lookup(im->root,a+8192)&(nx|2))==(nx|2));
    CHECK(cm_copyout(im,a+4096,&value,8)==CM_ERR_INVAL);
    cm_elf_destroy(child);child=NULL;
    CHECK(!cm_elf_mprotect(im,a,4096,3));
    CHECK(!cm_copyout(im,a,&value,8));
    kinfo("linux W^X mechanism verified: RWX=EPERM range-atomic RX/RO copyout denied shared=EOPNOTSUPP COW NX retained");
cleanup:
    cm_elf_destroy(child);cm_elf_destroy(im);cm_commit_stats(&after,&limit);
    if(before!=after || pages!=cm_mm_free_pages()+cm_file_cache_pages()){kerr("linux W^X reclaim mismatch");failed++;}
    if(!failed)kinfo("linux W^X mechanism rollback/reclaim=ok");
    return failed;
}
#ifndef CAPMIX_HOSTED
static void close_output(void *data){(void)cm_vfs_close(data);}
static int relro_headers(cm_elf_image_t *im,uint64_t address,unsigned n)
{
    if(!n || n>CM_ELF_MAX_PHDRS)return -1;
    cm_elf64_phdr_t ph[CM_ELF_MAX_PHDRS];if(cm_copyin(im,ph,address,n*sizeof(*ph)))return -1;
    unsigned regions=0;
    /* DSO/interpreter PHDR vaddrs are relative to the ELF header. */
    uint64_t base=address==im->phdr?0:(address&~4095ull);
    for(unsigned i=0;i<n;i++)if(ph[i].type==0x6474e552u) {
        uint64_t start=(base+ph[i].vaddr)&~4095ull,end=base+ph[i].vaddr+ph[i].memsz;
        if(end<start || end>CM_LINUX_USER_MAX)return -1;
        for(uint64_t v=start;v<end;v+=4096) {
            uint64_t pte=cm_space_lookup(im->root,v),word=0;
            if(!(pte&1) || (pte&2) || !(pte&(1ull<<63)) || cm_copyout(im,v,&word,8)!=CM_ERR_INVAL)return -1;
        }
        regions++;
    }
    return (int)regions;
}
static int hardened_image(cm_elf_image_t *im)
{
    int relro=relro_headers(im,im->phdr,im->phnum);if(relro<0)return 1;
    for(unsigned i=0;i<im->npages;i++) {
        cm_elf_page_t *p=cm_elf_page_at(im,i);uint64_t pte=cm_space_lookup(im->root,p->virt),nx=1ull<<63;
        if(!(pte&1) || ((pte&2) && !(pte&nx)) || (!!(p->flags&1)==!!(pte&nx)))return 1;
        if(p->virt>=CM_ELF_INTERP_BASE) {
            cm_elf64_ehdr_t *eh=cm_elf_pointer(im,p->virt);
            if(eh && !memcmp(eh->ident,"\177ELF",4) && eh->type==3) {
                if(eh->phoff>4096 || !eh->phnum || eh->phnum>CM_ELF_MAX_PHDRS)return 1;
                int n=relro_headers(im,p->virt+eh->phoff,eh->phnum);if(n<0)return 1;relro+=n;
            }
        }
    }
    uint64_t word=0;
    if(cm_copyout(im,im->main_entry,&word,8)!=CM_ERR_INVAL)return 1;
    kinfo("linux W^X/RELRO PTE verified: pages=%u relro=%d RX nonwritable data/stack NX copyout=denied",im->npages,relro);
    return 0;
}
static int execute_path(const char *path,const char *label,const char *expected,int code)
{
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;cm_file_t *file=NULL;cm_object_t *output=NULL;
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),before,limit,after;cm_commit_stats(&before,&limit);unsigned files=cm_vfs_open_files();
    CHECK(cm_vfs_open("/ldso-captured",CM_O_CREAT|CM_O_TRUNC|CM_O_RDWR,&file)==0);
    output=cm_obj_mint_owned(CM_OBJ_FILE,file,CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE,close_output);CHECK(output);file=NULL;
    cm_spawn_attr_t attr={.path=path,.qos=CM_QOS_USER_INTERACTIVE,.stdio={NULL,output}};
    CHECK(cm_posix_spawn(&attr,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*3;
    while((!p->exited || p->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    kinfo("linux ldso state %s: exited=%d code=%lld ENOSYS=%llu last=%llu",label,p->exited,(long long)p->exit_code,(unsigned long)p->linux_enosys,(unsigned long)p->linux_last_syscall);
    CHECK(p->exited && !p->main->id && p->exit_code==code && p->linux_enosys==0);
    if(!code)CHECK(!hardened_image(p->elf));
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
static int execute(const char *label,const char *expected,int code)
{return execute_path(MAIN,label,expected,code);}
#define TLS_MAIN "/bin/linux-tls.elf"
#define TLS_A "/lib/libcm_tls_a.so.1"
#define TLS_B "/lib/libcm_tls_b.so.1"
#define TLS_LOADS "ldso: loaded /lib/libcm_tls_a.so.1\nldso: loaded /lib/libcm_tls_b.so.1\n"
static int tls_overlap(void)
{
    int failed=0;cm_id_t pid[2]={0};cm_proc_t *p[2]={0};
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),before,limit,after;
    cm_commit_stats(&before,&limit);unsigned files=cm_vfs_open_files();
    cm_spawn_attr_t a={.path=TLS_MAIN,.qos=CM_QOS_USER_INTERACTIVE};
    for(unsigned i=0;i<2;i++){CHECK(!cm_posix_spawn(&a,&pid[i]));p[i]=cm_proc_lookup(pid[i]);CHECK(p[i]);}
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*3;
    while((!p[0]->exited || p[0]->main->id || !p[1]->exited || p[1]->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    for(unsigned i=0;i<2;i++)CHECK(p[i]->exited && !p[i]->main->id && !p[i]->exit_code && !p[i]->linux_enosys && p[i]->elf_preempts);
    CHECK(CM_MIN(p[0]->elf_end_tsc,p[1]->elf_end_tsc)>CM_MAX(p[0]->elf_start_tsc,p[1]->elf_start_tsc));
    CHECK(!cm_arch_user_fs_get());
    kinfo("linux tls isolation verified: same-cpu overlap preempts=%llu/%llu scheduler_FS=0",(unsigned long)p[0]->elf_preempts,(unsigned long)p[1]->elf_preempts);
cleanup:
    for(unsigned i=0;i<2;i++)if(p[i]){if(!p[i]->exited || p[i]->main->id){cm_proc_finish_exit(p[i],137);if(p[i]->main->id)cm_thread_reap(p[i]->main);}if(cm_proc_release_elf(pid[i]))failed++;}
    cm_commit_stats(&after,&limit);
    if(before!=after || pages!=cm_mm_free_pages()+cm_file_cache_pages() || files!=cm_vfs_open_files()){kerr("linux tls reclaim mismatch");failed++;}
    if(!failed)kinfo("linux tls overlap rollback/reclaim=ok");
    return failed;
}
static int tls_selftest(void)
{
    int failed=0;void *mutant=NULL;
    CHECK(!stage(TLS_MAIN,cm_tls_main,sizeof(cm_tls_main)));
    CHECK(!stage(TLS_A,cm_tls_a,sizeof(cm_tls_a)) && !stage(TLS_B,cm_tls_b,sizeof(cm_tls_b)));
    failed+=execute_path(TLS_MAIN,"tls-golden",TLS_LOADS "ldso: reloc RELATIVE=0 GLOB_DAT=0 JUMP_SLOT=4 R64=0\nldso: tls init modules=2 TPOFF64=4\nldso: W^X active objects=4 RELRO=4 no-relro=0\nlinux tls: payload verified\n",0);
    failed+=tls_overlap();
    CHECK(sizeof(cm_tls_a)<=8*CM_PAGE_SIZE);mutant=cm_page_alloc(3);CHECK(mutant);
    for(unsigned test=0;test<4;test++) {
        memcpy(mutant,cm_tls_a,sizeof(cm_tls_a));
        cm_elf64_ehdr_t *eh=mutant;cm_elf64_phdr_t *ph=(void *)((char *)mutant+eh->phoff);unsigned changed=0;
        if(test<3) {
            for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==7){
                if(test==0)ph[i].filesz=ph[i].memsz+1;
                if(test==1)ph[i].memsz=65537;
                if(test==2)ph[i].align=8192;
                changed=1;break;
            }
        }else {
            uint64_t rela=0;
            for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==2){uint64_t *d=(void *)((char *)mutant+ph[i].offset);for(unsigned j=0;j<ph[i].filesz/16;j++)if(d[2*j]==7)rela=d[2*j+1];}
            for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==1 && rela>=ph[i].vaddr && rela-ph[i].vaddr+24<=ph[i].filesz){
                uint64_t *r=(void *)((char *)mutant+ph[i].offset+rela-ph[i].vaddr);r[1]=(r[1]&~0xffffffffull)|16;changed=1;break;}
        }
        CHECK(changed && !stage(TLS_A,mutant,sizeof(cm_tls_a)));
        const char *labels[]={"tls-bad-template","tls-limit","tls-alignment","tls-general-dynamic"};
        const char *expected[]={"ldso: reject TLS template file bounds\n","ldso: reject TLS template bounds/alignment\n","ldso: reject TLS template bounds/alignment\n",TLS_LOADS "ldso: unsupported relocation=16\nldso: reject relocation unsupported\n"};
        failed+=execute_path(TLS_MAIN,labels[test],expected[test],126);
    }
cleanup:
    if(mutant)cm_page_free(mutant,3);
    if(stage(TLS_A,cm_tls_a,sizeof(cm_tls_a)))failed++;
    kinfo("linux tls selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
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
    CHECK(sizeof(cm_ldso_interp)<=8*CM_PAGE_SIZE);
    memcpy(mutant,cm_ldso_interp,sizeof(cm_ldso_interp));changed=0;
    for(unsigned i=0;i+9<=sizeof(cm_ldso_interp);i++)if(!memcmp((char *)mutant+i,"CapMixWX",9)){((char *)mutant)[i]='X';changed=1;break;}
    CHECK(changed && !stage(CM_ELF_LDSO_PATH,mutant,sizeof(cm_ldso_interp)));
    failed+=rejected(MAIN,CM_ERR_NOTSUP);
    CHECK(!stage(CM_ELF_LDSO_PATH,cm_ldso_interp,sizeof(cm_ldso_interp)));
    kinfo("linux W^X loader protocol verified: stale interpreter rejected rollback=ok");
    failed+=wx_mechanism();
    kinfo("linux ldso kernel validation: malformed PT_DYNAMIC/TLS/missing interpreter rollback=ok");
#ifndef CAPMIX_HOSTED
    failed+=execute("golden","ldso: loaded /lib/libcmgold.so.1\nldso: reloc RELATIVE=1 GLOB_DAT=4 JUMP_SLOT=1 R64=1\nldso: W^X active objects=3 RELRO=3 no-relro=0\nlinux ldso: relocation verified\n",0);
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
    /* Runtime-loader rejection, malformed RELRO, and legacy no-RELRO policy. */
    memcpy(mutant,cm_ldso_lib,sizeof(cm_ldso_lib));changed=0;
    for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==1 && (ph[i].flags&1)){ph[i].flags|=2;changed=1;break;}
    CHECK(changed && !stage(LIB,mutant,sizeof(cm_ldso_lib)));
    failed+=execute("wx-load","ldso: reject W+X PT_LOAD\n",126);
    memcpy(mutant,cm_ldso_lib,sizeof(cm_ldso_lib));changed=0;
    for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==0x6474e552u){ph[i].memsz++;changed=1;break;}
    CHECK(changed && !stage(LIB,mutant,sizeof(cm_ldso_lib)));
    failed+=execute("bad-relro","ldso: loaded /lib/libcmgold.so.1\nldso: reloc RELATIVE=1 GLOB_DAT=4 JUMP_SLOT=1 R64=1\nldso: reject RELRO bounds/alignment\n",126);
    memcpy(mutant,cm_ldso_lib,sizeof(cm_ldso_lib));changed=0;
    for(unsigned i=0;i<eh->phnum;i++)if(ph[i].type==0x6474e552u){ph[i].type=0;changed=1;break;}
    CHECK(changed && !stage(LIB,mutant,sizeof(cm_ldso_lib)));
    failed+=execute("no-relro","ldso: loaded /lib/libcmgold.so.1\nldso: reloc RELATIVE=1 GLOB_DAT=4 JUMP_SLOT=1 R64=1\nldso: W^X active objects=3 RELRO=2 no-relro=1\nlinux ldso: relocation verified\n",0);
    CHECK(!stage(LIB,cm_ldso_lib,sizeof(cm_ldso_lib)));
    failed+=tls_selftest();
    failed+=execute("restored","ldso: loaded /lib/libcmgold.so.1\nldso: reloc RELATIVE=1 GLOB_DAT=4 JUMP_SLOT=1 R64=1\nldso: W^X active objects=3 RELRO=3 no-relro=0\nlinux ldso: relocation verified\n",0);
#else
    kinfo("linux tls hosted: runtime execution skipped; hardware test requires QEMU");
#endif
cleanup:
    if(mutant){cm_page_free(mutant,3);if(stage(LIB,cm_ldso_lib,sizeof(cm_ldso_lib)) || stage(CM_ELF_LDSO_PATH,cm_ldso_interp,sizeof(cm_ldso_interp)))failed++;}
    cm_elf_destroy(im);
    kinfo("linux ldso selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
