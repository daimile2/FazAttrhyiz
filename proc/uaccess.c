#include "capmix/uaccess.h"
#include <string.h>
/* NULL image denotes trusted native C-entry callers, never an ordinary ELF.
 * Images are eagerly backed and remain pinned until off-CPU process reap. */
int cm_user_access(const cm_elf_image_t *im,uint64_t address,uint64_t size,int write)
{
    if(size>CM_USER_IO_MAX || address+size<address)return 0;
    if(!size)return 1;
    return address && (!im || cm_elf_user_range(im,address,size,write?6:4));
}
static cm_error_t copy(const cm_elf_image_t *im,void *kernel,uint64_t user,uint64_t size,int out)
{
    if(!cm_user_access(im,user,size,out))return CM_ERR_INVAL;
    while(size) {
        uint64_t n=CM_PAGE_SIZE-(user&CM_PAGE_MASK);if(n>size)n=size;
        void *p=im?cm_elf_pointer(im,user):(void *)(uintptr_t)user;
        if(out)memcpy(p,kernel,(size_t)n);else memcpy(kernel,p,(size_t)n);
        kernel=(char *)kernel+n;user+=n;size-=n;
    }
    return CM_OK;
}
cm_error_t cm_copyin(const cm_elf_image_t *im,void *dst,uint64_t src,uint64_t n)
{return copy(im,dst,src,n,0);}
cm_error_t cm_copyout(const cm_elf_image_t *im,uint64_t dst,const void *src,uint64_t n)
{return copy(im,(void *)src,dst,n,1);}
cm_error_t cm_copyinstr(const cm_elf_image_t *im,char *dst,uint64_t src,size_t capacity)
{
    for(size_t i=0;i<capacity;i++) {
        if(src+i<src || cm_copyin(im,dst+i,src+i,1))return CM_ERR_INVAL;
        if(!dst[i])return i?CM_OK:CM_ERR_INVAL;
    }
    return CM_ERR_INVAL;
}
