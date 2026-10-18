/* Linux x86-64 syscall-only HTTP fixture. Shared hosts parser, no libc/DNS. */
#include "cm_hosts.h"
static long sc(long n,long a,long b,long c)
{
    long r;__asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory","cc");return r;
}
static long input(void *ctx,void *p,size_t n){return sc(0,*(long *)ctx,(long)p,(long)n);}
static unsigned length(const char *s){unsigned n=0;while(s[n])n++;return n;}
static int output(const char *s,unsigned n)
{
    while(n){long r=sc(1,1,(long)s,n);if(r<=0 || r>(long)n)return 0;s+=r;n-=(unsigned)r;}return 1;
}
static void say(const char *s){(void)output(s,length(s));}
int main(int argc,char **argv)
{
    if(argc!=3){say("usage: linux-hosts-http.elf HOST PORT (GET /hello.txt)\n");return 2;}
    uint32_t ip;long f=sc(2,(long)"/etc/hosts",0,0);int e;
    if(f<0)e=f==-2?cm_hosts_resolve(argv[1],&ip,0,0):CM_HOSTS_IO;
    else {e=cm_hosts_resolve(argv[1],&ip,input,&f);if(sc(3,f,0,0)<0 && !e)e=CM_HOSTS_IO;}
    if(e){say(cm_hosts_error(e));return 2;}
    unsigned port=0,digits=0;
    for(const char *p=argv[2];*p;p++){if(*p<'0'||*p>'9'||++digits>5)return 2;port=port*10+(unsigned)(*p-'0');}
    if(!port || port>65535)return 2;
    struct {uint16_t family;unsigned char port[2],ip[4],zero[8];} a={2,
        {(unsigned char)(port>>8),(unsigned char)port},
        {(unsigned char)(ip>>24),(unsigned char)(ip>>16),(unsigned char)(ip>>8),(unsigned char)ip},{0}};
    long fd=sc(41,2,1,0);if(fd<0)return 3;
    if(sc(42,fd,(long)&a,sizeof(a))){sc(3,fd,0,0);say("linux hosts HTTP connect failed\n");return 3;}
    const char req[]="GET /hello.txt HTTP/1.0\r\nHost: hosts-probe\r\nConnection: close\r\n\r\n";
    unsigned off=0;
    while(off<sizeof(req)-1){long n=sc(1,fd,(long)(req+off),sizeof(req)-1-off);if(n<=0){sc(3,fd,0,0);return 4;}off+=(unsigned)n;}
    static char b[512],status[12];unsigned total=0;int ok=0;
    for(;;){long n=sc(0,fd,(long)b,sizeof(b));if(n<0){sc(3,fd,0,0);return 5;}if(!n)break;
        for(unsigned i=0;i<(unsigned)n && total+i<12;i++)status[total+i]=b[i];
        if(total+(unsigned)n>=12)ok=status[0]=='H' && status[9]=='2' && status[10]=='0' && status[11]=='0';
        if(!output(b,(unsigned)n)){sc(3,fd,0,0);return 6;}
        total+=(unsigned)n;if(total>65536){sc(3,fd,0,0);return 7;}}
    if(sc(3,fd,0,0)<0 || !ok)return 8;
    say("\nlinux hosts HTTP status=200 received verified\n");return 0;
}
