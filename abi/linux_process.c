#include "capmix/abi.h"
#include "capmix/proc.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>
static int snapshot(cm_elf_image_t *im,uint64_t address,char (*strings)[256],const char **out)
{
    if(!address){out[0]=NULL;return 0;}
    for(unsigned i=0;i<=32;i++) {
        uint64_t pointer;if(address>UINT64_MAX-i*8 || cm_copyin(im,&pointer,address+i*8,8))return -14;
        if(!pointer){out[i]=NULL;return 0;}
        if(i==32)return -7;
        unsigned j;
        for(j=0;j<256;j++){if(cm_copyin(im,&strings[i][j],pointer+j,1))return -14;if(!strings[i][j])break;}
        if(j==256)return -7;
        out[i]=strings[i];
    }
    return -7;
}
int64_t cm_linux_execve(uint64_t path_address,uint64_t argv_address,uint64_t env_address)
{
    cm_proc_t *p=cm_proc_current();if(!p || !p->linux_frame)return -38;
    if(cm_linux_group(p)->linux_grouped)return -95;
    char path[CM_ELF_PATH_MAX];if(cm_copyinstr(p->elf,path,path_address,sizeof(path)))return -14;
    if(path[0]!='/')return -2;
    char (*strings)[256]=cm_page_alloc(2);if(!strings)return -12;
    const char *argv[33],*env[33];cm_elf_image_t *next=NULL;
    int e=snapshot(p->elf,argv_address,strings,argv);if(!e)e=snapshot(p->elf,env_address,strings+32,env);
    if(e)goto out;
    e=cm_elf_load(path,&next);if(e){e=e==CM_ERR_NOENT?-2:e==CM_ERR_NOMEM || e==CM_ERR_OVERCOMMIT?-12:-8;goto out;}
    if(next->abi!=CM_ABI_LINUX){e=-8;goto out;}
    e=cm_elf_set_args(next,argv,env);if(e){e=e==CM_ERR_NOMEM?-12:-7;goto out;}
    /* All fallible work is complete. Kernel arguments contain no old-image
     * pointers; the syscall frame lives in the shared entry window. */
    uint64_t irq=cm_irq_save();cm_elf_image_t *old=p->elf;
    for(unsigned fd=0;fd<CM_FD_MAX;fd++)if(p->linux_cloexec[fd] && p->fd[fd]>=0) {
        int64_t ignored;(void)cm_linux_file_call(3,fd,0,0,0,&ignored);
    }
    p->elf=next;next=NULL;p->path=p->elf->path;p->task->name=p->path;p->aspace_pages=p->elf->charged;
    memset(p->linux_frame,0,120);p->linux_frame->rcx=p->elf->entry;p->linux_frame->r11=0x202;
    p->linux_clear_tid=0;p->linux_fs=0;p->linux_resume=0;cm_usermode_replace_image();
    cm_elf_destroy(old);
    kinfo("linux L2 execve replace: pid=%llu path=%s entry=%llx FS=0 old-image reclaimed",(unsigned long)p->pid,p->path,(unsigned long)p->elf->entry);
    cm_irq_restore(irq);e=0;
out:
    cm_elf_destroy(next);cm_page_free(strings,2);return e;
}
