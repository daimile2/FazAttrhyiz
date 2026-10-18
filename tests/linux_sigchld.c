/* Linux x86-64 syscall-only SIGCHLD fixture; no signal trampoline. */
#include <stdint.h>
static long call(long nr,long a,long b,long c,long d)
{
    register long r10 __asm__("r10")=d;long r;
    __asm__ volatile("syscall":"=a"(r):"a"(nr),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory","cc");return r;
}
#define S(n,a,b,c,d) call(n,(long)(a),(long)(b),(long)(c),(long)(d))
static long action(uint64_t handler,uint64_t flags){uint64_t a[4]={handler,flags,0,0};return S(13,17,a,0,8);}
static void say(const char *s){unsigned n=0;while(s[n])n++;S(1,1,s,n,0);}
static volatile unsigned runs;
int main(void)
{
    runs++;
    uint64_t bit=1ull<<16,pending=0,zero=0;int status=0;
    if(S(14,2,&bit,0,8))return 1;
    /* Two exits coalesce to one pending bit; default disposition stays waitable. */
    for(unsigned i=0;i<2;i++) {
        long pid=S(57,0,0,0,0);if(pid<0)return 2;
        if(!pid){S(60,41+i,0,0,0);return 99;}
        if(action(1,0)!=-16)return 3; /* policy frozen with an unreaped child */
        if(S(61,pid,&status,0,0)!=pid || status!=(int)((41+i)<<8))return 4;
    }
    if(S(127,&pending,8,0,0) || pending!=bit)return 5;
    if(S(127,1,8,0,0)!=-14 || S(127,&pending,128,0,0)!=-22)return 6;
    if(S(14,2,&zero,0,8) || S(127,&pending,8,0,0) || pending)return 7;
    say("SIGCHLD guest: blocked pending/coalesce default-wait/status unblock-ignore verified\n");
    for(unsigned policy=1;policy<=2;policy++) {
        if(action(policy==1?1:0,policy==2?2:0) || S(14,2,&bit,0,8))return 8;
        long pid=S(57,0,0,0,0);if(pid<0)return 9;
        if(!pid){S(60,43,0,0,0);return 99;}
        long r=S(61,pid,&status,0,0);if(r!=-10)return 10;
        if(S(127,&pending,8,0,0) || pending!=(policy==2?bit:0))return 11;
        if(S(14,2,&zero,0,8))return 12;
    }
    if(action(0,0))return 13;
    uint64_t a[4]={0x400000,0,0,0};if(S(13,17,a,0,8)!=-95)return 14;
    if(S(15,0,0,0,0)!=-38)return 15;
    say("SIGCHLD guest: IGN/NOCLDWAIT ECHILD autoreap verified; handler rejected sigreturn=ENOSYS\n");return 0;
}
