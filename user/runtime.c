#include "runtime.h"
int uconsole=2,ucontrol=1,uboot=3;
long ucall(unsigned nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
    register uint64_t r10 __asm__("r10")=d;long result;
    __asm__ volatile("syscall":"=a"(result):"a"((uint64_t)nr),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory","cc");
    return result;
}
unsigned ulen(const char *s){unsigned n=0;while(s[n])n++;return n;}
int ueq(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
void uputs(const char *s){unsigned n=ulen(s);while(n){unsigned part=n>4096?4096:n;long e=ucall(32,uconsole,(uintptr_t)s,part,0);if(e==CM_ERR_AGAIN){uyield();continue;}if(e<0)return;s+=part;n-=part;}}
void unum(int64_t x){char b[24];unsigned n=0;uint64_t v=(uint64_t)x;if(x<0){uputs("-");v=0-v;}do{b[n++]=(char)('0'+v%10);v/=10;}while(v);for(unsigned i=0;i<n/2;i++){char t=b[i];b[i]=b[n-1-i];b[n-1-i]=t;}b[n]=0;uputs(b);}
void uyield(void){(void)ucall(CM_SYS_YIELD,0,0,0,0);}
int ucpl(void){unsigned short cs;__asm__ volatile("mov %%cs,%0":"=r"(cs));return cs&3;}
long uspawn(const char *path,const char *const *argv,const char *const *envp,unsigned flags,cm_user_child_t *out)
{
    cm_user_spawn_t r={.path=(uintptr_t)path,.argv=(uintptr_t)argv,.envp=(uintptr_t)envp,.flags=flags,.cpu=-1};
    return ucall(3,ucontrol,(uintptr_t)&r,(uintptr_t)out,0);
}
long uwait(int h,cm_user_wait_t *result,unsigned flags){return ucall(33,h,(uintptr_t)result,flags,0);}
void *memcpy(void *dst,const void *src,size_t n){unsigned char *d=dst;const unsigned char *s=src;while(n--)*d++=*s++;return dst;}
void *memset(void *dst,int c,size_t n){unsigned char *d=dst;while(n--)*d++=(unsigned char)c;return dst;}
size_t strlen(const char *s){return ulen(s);}
