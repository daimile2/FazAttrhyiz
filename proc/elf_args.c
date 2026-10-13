#include "capmix/elf.h"
#include "capmix/mm.h"
#include "capmix/uaccess.h"
#include <string.h>
_Static_assert(CM_ELF_ARG_BYTES<=CM_PAGE_SIZE,"argument snapshot fits one page");
/* Snapshot strings once; sources may be PID1's temporary line/token arrays. */
static cm_error_t vector(const char *const *source,unsigned limit,char *bytes,
                         unsigned *used,uint16_t *offsets,unsigned *count)
{
    *count=0;if(!source)return CM_OK;
    while(*count<limit && source[*count]) {
        const char *text=source[*count];unsigned start=*used,n=0;
        for(;;) {
            if(n==CM_ELF_ARG_STR_MAX || *used==CM_ELF_ARG_BYTES)return CM_ERR_NOSPC;
            char c=text[n++];bytes[(*used)++]=c;if(!c)break;
        }
        offsets[(*count)++]=(uint16_t)start;
    }
    return source[*count]?CM_ERR_NOSPC:CM_OK;
}
cm_error_t cm_elf_set_args(cm_elf_image_t *im,const char *const *argv,const char *const *envp)
{
    if(!im)return CM_ERR_INVAL;
    const char *defaults[]={im->path,NULL};if(!argv || !argv[0])argv=defaults;
    char *bytes=cm_page_alloc(0);if(!bytes)return CM_ERR_NOMEM;
    uint16_t args[CM_ELF_ARG_MAX],env[CM_ELF_ENV_MAX];unsigned used=0,argc=0,envc=0;
    cm_error_t e=vector(argv,CM_ELF_ARG_MAX,bytes,&used,args,&argc);
    if(!e)e=vector(envp,CM_ELF_ENV_MAX,bytes,&used,env,&envc);
    if(e)goto done;
    uint64_t strings=CM_ELF_STACK_TOP-used;
    unsigned words=argc+envc+5;uint64_t rsp=(strings-words*8u)&~15ull;
    uint64_t bottom=CM_ELF_STACK_TOP-CM_ELF_STACK_PAGES*CM_PAGE_SIZE;
    if(rsp<bottom || !cm_elf_user_range(im,bottom,CM_ELF_STACK_PAGES*CM_PAGE_SIZE,6)){e=CM_ERR_INVAL;goto done;}
    uint64_t table[CM_ELF_ARG_MAX+CM_ELF_ENV_MAX+5];unsigned n=0;
    table[n++]=argc;
    for(unsigned i=0;i<argc;i++)table[n++]=strings+args[i];
    table[n++]=0;
    for(unsigned i=0;i<envc;i++)table[n++]=strings+env[i];
    table[n++]=0;table[n++]=0;table[n++]=0; /* auxv AT_NULL,value=0 */
    /* No thread may be running this image. Validate before any stack change. */
    for(unsigned i=0;i<im->npages;i++)if(im->pages[i].virt>=bottom && im->pages[i].virt<CM_ELF_STACK_TOP)
        memset(im->pages[i].backing,0,CM_PAGE_SIZE);
    e=cm_copyout(im,strings,bytes,used);
    if(!e)e=cm_copyout(im,rsp,table,n*8u);
    if(!e){im->stack=rsp;im->argc=argc;im->envc=envc;}
done:
    cm_page_free(bytes,0);return e;
}
