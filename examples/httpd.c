#include "capmix.h"
/* One bounded serial worker per process. Optional ID labels a 1..4 process
 * shard launched by shell; no fork, spawn authority or socket handoff. */
static const char response[]=
    "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 30\r\n"
    "Connection: close\r\n\r\nhello from FazAttrhyiz httpd!\n";
_Static_assert(sizeof("hello from FazAttrhyiz httpd!\n")-1==30,"HTTP content length");
struct timings {uint64_t read,send;};
static int serve(long h,uint64_t hz,struct timings *t)
{
    char request[2048];size_t used=0;int complete=0;
    uint64_t start=cm_now(),end=start+hz*3;
    while(cm_now()<end && used<sizeof(request)) {
        long n=cm_tcp_read(h,request+used,sizeof(request)-used);
        if(n==CM_EAGAIN){cm_yield();continue;}
        if(n<=0){cm_report("httpd request error",n);return 1;}
        size_t old=used;used+=(size_t)n;
        for(size_t i=old>3?old-3:0;i+3<used;i++)
            if(request[i]=='\r' && request[i+1]=='\n' && request[i+2]=='\r' && request[i+3]=='\n')complete=1;
        if(complete)break;
    }
    if(!complete){cm_print("httpd request rejected: header incomplete/too large/timeout\n");return 1;}
    /* Only a minimal GET request line. Header contents are otherwise ignored. */
    if(used<16 || request[0]!='G' || request[1]!='E' || request[2]!='T' || request[3]!=' ' || request[4]!='/') {
        cm_print("httpd request rejected: GET required\n");return 1;
    }
    uint64_t ready=cm_now();t->read=ready-start;
    long e=cm_tcp_write(h,response,sizeof(response)-1);t->send=cm_now()-ready;
    if(e<0){cm_report("httpd send error",e);return 1;}
    cm_print("httpd response: HTTP/1.0 200 payload=30 verified\n");return 0;
}
static void worker_record(const char *event,unsigned id,unsigned port,unsigned handled,int status)
{
    char b[120];size_t at=0;const char *parts[]={"httpd worker ",event,": id=",NULL};
    for(unsigned i=0;parts[i];i++)for(const char *p=parts[i];*p;p++)b[at++]=*p;
    unsigned values[]={id,port,handled,(unsigned)status};
    const char *labels[]={" port="," handled="," status=","\n"};
    for(unsigned i=0;i<4;i++){char digits[12];unsigned n=0,v=values[i];do{digits[n++]=(char)('0'+v%10);v/=10;}while(v);while(n)b[at++]=digits[--n];for(const char *p=labels[i];*p;p++)b[at++]=*p;}
    b[at]=0;cm_print(b);
}
static void profile_record(struct timings t,uint64_t close,uint64_t hz)
{
    char b[160];size_t at=0;const char *labels[]={"httpd profile read_us="," send_ack_us="," close_us="};
    uint64_t values[]={t.read,t.send,close};
    for(unsigned i=0;i<3;i++) {
        for(const char *p=labels[i];*p;p++)b[at++]=*p;
        uint64_t v=values[i]*1000000/hz;char digits[24];unsigned n=0;
        do{digits[n++]=(char)('0'+v%10);v/=10;}while(v);while(n)b[at++]=digits[--n];
    }
    b[at++]='\n';b[at]=0;cm_print_fd(2,b);
}
int main(int argc,char **argv,char **envp)
{
    (void)envp;uint16_t port=8080;unsigned limit=0,worker=0;
    if(argc>5 || (argc>=2 && !cm_parse_port(argv[1],&port))) {
        cm_print("usage: run /bin/httpd.elf [PORT [REQUESTS [WORKER_ID]]] [&]\n");return 2;
    }
    if(argc>=3) {
        uint16_t n;if(!cm_parse_port(argv[2],&n) || n>100){cm_print("httpd: REQUESTS must be 1..100\n");return 2;}limit=n;
    }
    if(argc>=4){uint16_t n;if(!cm_parse_port(argv[3],&n) || n>4){cm_print("httpd: WORKER_ID must be 1..4\n");return 2;}worker=n;}
    int profile=argc==5;if(profile && !cm_equal(argv[4],"1"))return 2;
    uint64_t hz=cm_hz();if(!hz)return 1;
    long listener=cm_tcp_serve(port,4);if(listener<0){cm_report("httpd listen error",listener);return 1;}
    cm_report("httpd listening port",port);cm_print("httpd accept=nonblocking backlog=4 serial=1\n");
    if(worker)worker_record("ready",worker,port,0,0);
    unsigned handled=0;int status=0;
    for(;;) {
        long h=cm_tcp_accept(listener);
        if(h==CM_EAGAIN){cm_yield();continue;}
        if(h<0){cm_report("httpd accept error",h);status=1;break;}
        cm_print("httpd accepted\n");
        struct timings t={0};int failed=serve(h,hz,&t);uint64_t close=cm_now();long e=cm_tcp_hangup(h);close=cm_now()-close;
        if(profile)profile_record(t,close,hz);
        if(e<0)cm_report("httpd close error",e);
        cm_report("httpd handled",++handled);
        status|=failed || e<0;
        if(limit && handled>=limit)break;
    }
    long e=cm_tcp_hangup(listener);if(e<0)status=1;
    if(worker)worker_record("done",worker,port,handled,status);
    cm_print("httpd stopped: listener closed\n");return status;
}
