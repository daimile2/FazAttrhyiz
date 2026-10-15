#ifndef CAPMIX_SDK_H
#define CAPMIX_SDK_H
#if !defined(__x86_64__)
#error "CapMix SDK requires an x86_64 GCC target"
#endif
#include <stddef.h>
#include <stdint.h>
/* CapMix x86_64 ABI, not Linux syscall numbers. Negative errors are returned
 * directly. No errno/TLS/libc. Fourth argument is R10; RCX/R11 are clobbered. */
#define CM_EXIT 1u
#define CM_WRITE 2u
static inline long cm_call(unsigned nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
    register uint64_t r10 __asm__("r10")=d;
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"((uint64_t)nr),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory","cc");
    return result;
}
/* stdout/stderr are diagnostic log streams, each write <=127 bytes. Return
 * 0 on complete output, negative error otherwise; syscall WRITE returns bytes. */
static inline long cm_print_fd(unsigned fd,const char *text)
{
    while(*text){size_t n=0;while(n<127 && text[n])n++;
        long e=cm_call(CM_WRITE,fd,(uintptr_t)text,n,0);if(e<=0)return e?e:-11;text+=(size_t)e;}
    return 0;
}

static inline long cm_print(const char *text){return cm_print_fd(1,text);}

#define CM_EAGAIN (-5L)
#define CM_ENOENT (-12L)
#define CM_OPEN 11u
#define CM_READ 12u
#define CM_CLOSE 13u
#define CM_TCP_OPEN 26u
#define CM_TCP_CONNECT 27u
#define CM_TCP_SEND 28u
#define CM_TCP_RECV 29u
#define CM_TCP_CLOSE 30u
#define CM_TCP_LISTEN 40u
#define CM_TCP_ACCEPT 41u
#define CM_CONSOLE_READ 31u
#define CM_CONSOLE_WRITE 32u
#define CM_YIELD 34u
#define CM_READDIR 35u
#define CM_CLOCK 8u
#define CM_TSC_HZ 39u
#define CM_INTERACTIVE_CONSOLE 1u
static inline size_t cm_length(const char *s){size_t n=0;while(s[n])n++;return n;}
static inline int cm_equal(const char *a,const char *b)
{while(*a && *a==*b){a++;b++;}return *a==*b;}
static inline void cm_yield(void){(void)cm_call(CM_YIELD,0,0,0,0);}
static inline uint64_t cm_now(void){return (uint64_t)cm_call(CM_CLOCK,0,0,0,0);}
static inline uint64_t cm_hz(void){long n=cm_call(CM_TSC_HZ,0,0,0,0);return n>0?(uint64_t)n:0;}
static inline long cm_open(const char *path){return cm_call(CM_OPEN,(uintptr_t)path,0,0,0);}
static inline long cm_read(long fd,void *p,size_t n){return cm_call(CM_READ,fd,(uintptr_t)p,n,0);}
static inline long cm_close(long fd){return cm_call(CM_CLOSE,fd,0,0,0);}
static inline long cm_output(const void *data,size_t n)
{
    const unsigned char *p=data;
    while(n){size_t part=n<127?n:127;long e=cm_call(CM_WRITE,1,(uintptr_t)p,part,0);if(e<=0)return e?e:-11;p+=(size_t)e;n-=(size_t)e;}
    return 0;
}
static inline long cm_console_write(const void *p,size_t n)
{
    while(n){long e=cm_call(CM_CONSOLE_WRITE,CM_INTERACTIVE_CONSOLE,(uintptr_t)p,n,0);
        if(e==CM_EAGAIN){cm_yield();continue;}if(e<0)return e;if(!e)return -11;
        p=(const char *)p+e;n-=(size_t)e;}
    return 0;
}
static inline long cm_console_read(char *p)
{return cm_call(CM_CONSOLE_READ,CM_INTERACTIVE_CONSOLE,(uintptr_t)p,1,0);}
static inline long cm_console_probe(void)
{return cm_call(CM_CONSOLE_WRITE,CM_INTERACTIVE_CONSOLE,0,0,0);}
static inline long cm_report(const char *label,long error)
{
    char b[128],digits[24];size_t n=0,k=0;uint64_t v=(uint64_t)error;
    while(*label && n<90)b[n++]=*label++;
    b[n++]='=';if(error<0){b[n++]='-';v=0-v;}
    do{digits[k++]=(char)('0'+v%10);v/=10;}while(v);
    while(k)b[n++]=digits[--k];
    b[n++]='\n';b[n]=0;return cm_print_fd(2,b);
}
typedef struct {uint32_t ifid,address;uint16_t port,reserved;uint32_t timeout_ms;} cm_sdk_tcp_request_t;
typedef struct {uint64_t ino;uint32_t kind;char name[58];} cm_sdk_dirent_t;
_Static_assert(sizeof(cm_sdk_tcp_request_t)==16 && sizeof(cm_sdk_dirent_t)==72,"CapMix SDK ABI sizes");
typedef struct {uint32_t ifid;uint16_t port,backlog;} cm_sdk_tcp_listen_t;
_Static_assert(sizeof(cm_sdk_tcp_listen_t)==8,"CapMix TCP listen ABI");
static inline long cm_tcp_serve(uint16_t port,unsigned backlog)
{
    if(backlog<1 || backlog>4)return -2;
    long h=cm_call(CM_TCP_OPEN,0,0,0,0);if(h<0)return h;
    cm_sdk_tcp_listen_t r={UINT32_MAX,port,(uint16_t)backlog};
    long e=cm_call(CM_TCP_LISTEN,h,(uintptr_t)&r,0,0);
    if(e<0){(void)cm_call(CM_TCP_CLOSE,h,100,0,0);return e;}return h;
}
/* Empty accept queue: CM_EAGAIN. Caller yields or applies its own deadline. */
static inline long cm_tcp_accept(long h){return cm_call(CM_TCP_ACCEPT,h,0,0,0);}
static inline long cm_tcp_dial(uint32_t ip,uint16_t port)
{
    long h=cm_call(CM_TCP_OPEN,0,0,0,0);if(h<0)return h;
    cm_sdk_tcp_request_t r={.ifid=UINT32_MAX,.address=ip,.port=port,.timeout_ms=3000};
    long e=cm_call(CM_TCP_CONNECT,h,(uintptr_t)&r,0,0);
    if(e<0){(void)cm_call(CM_TCP_CLOSE,h,100,0,0);return e;}return h;
}
static inline long cm_tcp_write(long h,const void *p,size_t n)
{
    while(n){size_t part=n<4096?n:4096;long e=cm_call(CM_TCP_SEND,h,(uintptr_t)p,part,3000);
        if(e<=0)return e?e:-11;
        p=(const char *)p+e;n-=(size_t)e;}
    return 0;
}
static inline long cm_tcp_read(long h,void *p,size_t n){return cm_call(CM_TCP_RECV,h,(uintptr_t)p,n,0);}
static inline long cm_tcp_hangup(long h){return cm_call(CM_TCP_CLOSE,h,3000,0,0);}
static inline int cm_parse_port(const char *s,uint16_t *out)
{
    unsigned n=0;if(!*s)return 0;
    while(*s){if(*s<'0' || *s>'9')return 0;n=n*10+(unsigned)(*s++-'0');if(n>65535)return 0;}
    if(!n)return 0;
    *out=(uint16_t)n;return 1;
}
static inline int cm_parse_ipv4(const char *s,uint32_t *out)
{
    uint32_t ip=0;
    for(unsigned i=0;i<4;i++){unsigned n=0,k=0;while(*s>='0' && *s<='9'){n=n*10+(unsigned)(*s++-'0');if(n>255 || ++k>3)return 0;}
        if(!k || (i<3?*s++!='.':*s!=0))return 0;
        ip=(ip<<8)|n;}
    if(!ip || ip==UINT32_MAX)return 0;
    *out=ip;return 1;
}
#endif
