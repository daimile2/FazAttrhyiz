/* Fixed no-libc Linux x86-64 fixture; no CapMix syscall numbers. */
typedef unsigned long u64;
typedef struct {unsigned short family;unsigned char port[2],ip[4],zero[8];} address;
static long sc(long nr,long a,long b,long c,long d,long e,long f)
{
    register long r10 __asm__("r10")=d,r8 __asm__("r8")=e,r9 __asm__("r9")=f;long out;
    __asm__ volatile("syscall":"=a"(out):"a"(nr),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","cc","memory");return out;
}
#define S(n,a,b,c) sc(n,(long)(a),(long)(b),(long)(c),0,0,0)
static unsigned len(const char *s){unsigned n=0;while(s[n])n++;return n;}
static int eq(const void *a,const void *b,unsigned n){const unsigned char *x=a,*y=b;for(unsigned i=0;i<n;i++)if(x[i]!=y[i])return 0;return 1;}
static void say(const char *s){(void)S(1,1,s,len(s));}
static int all(long fd,const void *p,unsigned n)
{
    const char *s=p;while(n){long r=S(1,fd,s,n);if(r<=0 || (u64)r>n)return 0;s+=r;n-=(unsigned)r;}return 1;
}
static address host(unsigned port){address a={2,{(unsigned char)(port>>8),(unsigned char)port},{10,0,2,2},{0}};return a;}
static address loop(unsigned port){address a={2,{(unsigned char)(port>>8),(unsigned char)port},{127,0,0,1},{0}};return a;}
static unsigned char sent[2049],got[8192];
static int echo_client(int no_nic)
{
    long fd=S(41,2,1,0);if(fd<0)return 10;
    if(S(33,fd,20,0)!=20 || S(3,fd,0,0))return 11;
    fd=20;
    address a=host(9998);long e=S(42,fd,&a,16);
    if(no_nic){if(e!=-101)return 12;(void)S(3,fd,0,0);say("linux L3 no NIC: ENETUNREACH verified\n");return 0;}
    if(e)return 13;
    address peer={0};unsigned size=16;
    if(S(52,fd,&peer,&size) || size!=16 || !eq(&peer,&a,8))return 14;
    for(unsigned i=0;i<sizeof(sent);i++)sent[i]=(unsigned char)(i*37+17);
    if(!all(fd,sent,sizeof(sent)))return 15;
    unsigned used=0;
    while(used<sizeof(sent)){long n=S(0,fd,got+used,sizeof(sent)-used);if(n<=0)return 16;used+=(unsigned)n;}
    if(!eq(sent,got,sizeof(sent)))return 17;
    if(S(72,fd,4,0x802) || S(0,fd,got,1)!=-11 || S(72,fd,4,2))return 18;
    if(S(8,fd,0,0)!=-29 || S(3,fd,0,0))return 19;
    say("linux L3 TCP client payload=2049 verified dup-close=ok\n");return 0;
}
static int udp(void)
{
    long fd=S(41,2,2,0);if(fd<0)return 20;address a=loop(19090),peer={0};unsigned size=16;
    if(S(49,fd,&a,16))return 21;
    for(unsigned i=0;i<1024;i++)sent[i]=(unsigned char)(i*11+3);
    if(sc(44,fd,(long)sent,1024,0,(long)&a,16)!=1024)return 22;
    if(S(0,fd,1,1024)!=-14)return 23; /* must not consume datagram */
    if(sc(45,fd,(long)got,sizeof(got),0,(long)&peer,(long)&size)!=1024 || size!=16 || !eq(&peer,&a,8) || !eq(sent,got,1024))return 24;
    if(S(42,fd,&a,16) || !all(fd,sent,256) || S(0,fd,got,256)!=256 || !eq(sent,got,256))return 25;
    if(S(3,fd,0,0))return 26;
    say("linux L3 UDP lo payload=1024 verified six-arg sendto/recvfrom + connected read/write\n");return 0;
}
static int contains(const unsigned char *p,unsigned n,const char *s)
{
    unsigned m=len(s);for(unsigned i=0;i+m<=n;i++)if(eq(p+i,s,m))return 1;return 0;
}
static int http_client(void)
{
    long fd=S(41,2,1,0);if(fd<0)return 30;address a=host(8000);
    if(S(42,fd,&a,16))return 31;
    static const char req[]="GET /hello.txt HTTP/1.0\r\nHost: test\r\n\r\n";
    if(!all(fd,req,sizeof(req)-1))return 32;
    unsigned used=0;
    while(used<sizeof(got)){long n=S(0,fd,got+used,sizeof(got)-used);if(n<0)return 33;if(!n)break;used+=(unsigned)n;}
    if(used<12 || (!eq(got,"HTTP/1.0 200",12) && !eq(got,"HTTP/1.1 200",12)) || !contains(got,used,"linux L3 host body\n"))return 34;
    if(S(3,fd,0,0))return 35;
    say("linux L3 HTTP client status=200 body verified\n");return 0;
}
static int http_server(void)
{
    long fd=S(41,2,1,0);if(fd<0)return 40;address a={2,{0x1f,0x92},{0},{0}}; /* 8082 */
    if(S(49,fd,&a,16) || S(50,fd,4,0))return 41;
    if(S(33,fd,21,0)!=21 || S(3,fd,0,0))return 42;
    fd=21;
    say("linux L3 server ready port=8082\n");address peer={0};unsigned size=16;long child=-11;
    for(unsigned i=0;i<10 && child==-11;i++)child=S(43,fd,&peer,&size);
    if(child<0 || size!=16 || peer.family!=2 || !(peer.port[0]|peer.port[1]))return 43;
    if(S(33,child,20,0)!=20 || S(3,child,0,0))return 44;
    unsigned used=0;
    while(used<4096 && !contains(got,used,"\r\n\r\n")){long n=S(0,20,got+used,4096-used);if(n<=0)return 45;used+=(unsigned)n;}
    if(!contains(got,used,"\r\n\r\n") || !eq(got,"GET ",4))return 46;
    static const char response[]="HTTP/1.0 200 OK\r\nContent-Length: 25\r\nConnection: close\r\n\r\nhello from Linux ABI L3!\n";
    if(!all(20,response,sizeof(response)-1) || S(3,20,0,0) || S(3,fd,0,0))return 47;
    say("linux L3 HTTP server response verified\n");return 0;
}
static int roundtrip(long fd)
{
    static const char text[]="Linux fork socket remains alive";
    if(!all(fd,text,sizeof(text)))return 0;
    unsigned n=0;while(n<sizeof(text)){long r=S(0,fd,got+n,sizeof(text)-n);if(r<=0)return 0;n+=(unsigned)r;}
    return eq(got,text,sizeof(text));
}
static int exec_check(void)
{
    unsigned char stat[144];
    if(S(5,3,stat,0)!=-9 || !roundtrip(4) || S(3,4,0,0))return 50;
    say("linux L3 exec CLOEXEC socket verified; non-CLOEXEC stream retained\n");return 0;
}
static int inherit(void)
{
    long fd=S(41,2,1|0x80000,0);if(fd!=3)return 51;address a=host(9998);
    if(S(42,fd,&a,16) || S(33,fd,4,0)!=4)return 52;
    long child=S(57,0,0,0);if(child<0)return 53;
    if(!child){(void)S(3,fd,0,0);(void)S(60,0,0,0);return 54;}
    unsigned status=1;if(S(61,child,&status,0)!=child || status || !roundtrip(fd))return 55;
    child=S(57,0,0,0);if(child<0)return 56;
    if(!child){const char *args[]={"/bin/linux-net.elf","exec-check",0};(void)S(59,args[0],args,0);(void)S(60,57,0,0);return 57;}
    status=1;if(S(61,child,&status,0)!=child || status || S(3,fd,0,0) || !roundtrip(4) || S(3,4,0,0))return 58;
    say("linux L3 TCP fork/exec aliases verified last-close=reclaimed\n");return 0;
}
static int mode(const char *a,const char *b){return len(a)==len(b) && eq(a,b,len(a));}
int main(long argc,char **argv)
{
    if(argc!=2){say("usage: linux-l3.elf echo|udp|http|server|nonet|inherit\n");return 2;}
    int status;
    if(mode(argv[1],"echo"))status=echo_client(0);
    else if(mode(argv[1],"udp"))status=udp();
    else if(mode(argv[1],"http"))status=http_client();
    else if(mode(argv[1],"server"))status=http_server();
    else if(mode(argv[1],"nonet"))status=echo_client(1);
    else if(mode(argv[1],"inherit"))status=inherit();
    else if(mode(argv[1],"exec-check"))status=exec_check();
    else return 2;
    if(status)say("linux L3 fixture FAILED (see exit code)\n");
    return status;
}
