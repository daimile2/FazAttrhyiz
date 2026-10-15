/* Exercise the custom user's allocator without replacing the host libc heap. */
#define malloc arena_malloc
#define free arena_free
#define calloc arena_calloc
#define realloc arena_realloc
#define __libc_malloc arena_libc_malloc
#define __libc_malloc_impl arena_libc_malloc_impl
#define __libc_calloc arena_libc_calloc
#include "../user/linux/bss_alloc.c"
int main(void)
{
    void *p[64];unsigned n=0;
    while(n<64 && (p[n]=malloc(1024))){memset(p[n],(int)n,1024);n++;}
    if(n<60 || n==64 || errno!=ENOMEM)return 1;
    for(unsigned i=0;i<n;i++)for(unsigned j=0;j<1024;j++)if(((unsigned char *)p[i])[j]!=i)return 2;
    /* Failure preserves the old allocation and data. */
    if(realloc(p[0],65536)!=NULL || ((unsigned char *)p[0])[0]!=0)return 3;
    for(unsigned i=0;i<n;i+=2)free(p[i]);
    for(unsigned i=1;i<n;i+=2)free(p[i]);
    void *large=malloc(65504);if(!large)return 4;free(large);
    volatile size_t overflow=SIZE_MAX;
    if(calloc(overflow,2)!=NULL || errno!=ENOMEM)return 5;
    unsigned char *zero=calloc(128,8);if(!zero)return 6;
    for(unsigned i=0;i<1024;i++)if(zero[i])return 7;
    free(zero);return 0;
}
