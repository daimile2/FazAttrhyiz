/* CapMix L1: bounded 64KiB per-process BSS allocator. Single thread only.
 * Interposes musl's public and internal malloc entry points, never mmap/brk.
 * Original CapMix code; see musl-integer-printf.patch for libc changes. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#define ARENA_SIZE (64u*1024u)
static _Alignas(16) unsigned char arena[ARENA_SIZE];
typedef struct {size_t bytes,used;} block;
static unsigned initialized;
static void init(void){if(!initialized){((block *)arena)->bytes=ARENA_SIZE-sizeof(block);initialized=1;}}
void *malloc(size_t n)
{
    if(n>ARENA_SIZE-32){errno=ENOMEM;return NULL;}
    n=(n+15u)&~(size_t)15u;if(!n)n=16;init();
    for(size_t at=0;at<ARENA_SIZE;) {
        block *b=(void *)(arena+at);
        if(!b->used && b->bytes>=n) {
            if(b->bytes>=n+sizeof(block)+16) {
                block *tail=(void *)(arena+at+sizeof(block)+n);
                tail->bytes=b->bytes-n-sizeof(block);tail->used=0;b->bytes=n;
            }
            b->used=1;return b+1;
        }
        at+=sizeof(block)+b->bytes;
    }
    errno=ENOMEM;return NULL;
}
void free(void *p)
{
    if(!p)return;init();
    for(size_t at=0;at<ARENA_SIZE;) {
        block *b=(void *)(arena+at);if((void *)(b+1)==p){b->used=0;break;}
        at+=sizeof(block)+b->bytes;
    }
    for(size_t at=0;at<ARENA_SIZE;) {
        block *b=(void *)(arena+at);size_t next=at+sizeof(block)+b->bytes;
        if(next<ARENA_SIZE && !b->used) {
            block *c=(void *)(arena+next);
            if(!c->used){b->bytes+=sizeof(block)+c->bytes;continue;}
        }
        at=next;
    }
}
void *calloc(size_t n,size_t size)
{
    if(size && n>SIZE_MAX/size){errno=ENOMEM;return NULL;}
    size_t total=n*size;void *p=malloc(total);if(p)memset(p,0,total);return p;
}
void *realloc(void *p,size_t n)
{
    if(!p)return malloc(n);
    if(!n){free(p);return NULL;}
    block *b=(block *)p-1;if(n<=b->bytes)return p;
    void *q=malloc(n);if(q){memcpy(q,p,b->bytes);free(p);}return q;
}
void *__libc_malloc(size_t n){return malloc(n);}
void *__libc_malloc_impl(size_t n){return malloc(n);}
void *__libc_calloc(size_t n,size_t size){return calloc(n,size);}
