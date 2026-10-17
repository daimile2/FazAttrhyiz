#ifndef CM_RTLD_RUNTIME_H
#define CM_RTLD_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
static inline long call6(long nr,long a,long b,long c,long d,long e,long f)
{
    register long r10 __asm__("r10")=d,r8 __asm__("r8")=e,r9 __asm__("r9")=f;long r;
    __asm__ volatile("syscall":"=a"(r):"a"(nr),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");return r;
}
static inline size_t length(const char *s){size_t n=0;while(s[n])n++;return n;}
static inline int equal(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static inline void bytes(void *d,const void *s,size_t n){char *p=d;const char *q=s;while(n--)*p++=*q++;}
static inline void text(const char *s){size_t n=length(s);while(n){long r=call6(1,1,(long)s,n,0,0,0);if(r<=0)break;s+=r;n-=r;}}
static inline void number(uint64_t n){char s[24];unsigned i=sizeof(s);do{s[--i]='0'+n%10;n/=10;}while(n);(void)call6(1,1,(long)(s+i),sizeof(s)-i,0,0,0);}
static inline void *allocate(size_t n){long r=call6(9,0,n,3,0x22,-1,0);return r<0?NULL:(void *)r;}
static inline void release(void *p,size_t n){if(p)(void)call6(11,(long)p,n,0,0,0,0);}
#endif
