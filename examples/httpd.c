#include "capmix.h"
/* A deliberately bounded serial HTTP/1.0 responder, not a general web server. */
static const char response[]=
    "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 25\r\n"
    "Connection: close\r\n\r\nhello from CapMix httpd!\n";
_Static_assert(sizeof("hello from CapMix httpd!\n")-1==25,"HTTP content length");
static int serve(long h,uint64_t hz)
{
    char request[2048];size_t used=0;int complete=0;
    uint64_t end=cm_now()+hz*3;
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
    long e=cm_tcp_write(h,response,sizeof(response)-1);
    if(e<0){cm_report("httpd send error",e);return 1;}
    cm_print("httpd response: HTTP/1.0 200 payload=25 verified\n");return 0;
}
int main(int argc,char **argv,char **envp)
{
    (void)envp;uint16_t port=8080;unsigned limit=0;
    if(argc>3 || (argc>=2 && !cm_parse_port(argv[1],&port))) {
        cm_print("usage: run /bin/httpd.elf [PORT [REQUESTS]] [&]\n");return 2;
    }
    if(argc==3) {
        uint16_t n;if(!cm_parse_port(argv[2],&n) || n>100){cm_print("httpd: REQUESTS must be 1..100\n");return 2;}limit=n;
    }
    uint64_t hz=cm_hz();if(!hz)return 1;
    long listener=cm_tcp_serve(port,4);if(listener<0){cm_report("httpd listen error",listener);return 1;}
    cm_report("httpd listening port",port);cm_print("httpd accept=nonblocking backlog=4 serial=1\n");
    unsigned handled=0;int status=0;
    for(;;) {
        long h=cm_tcp_accept(listener);
        if(h==CM_EAGAIN){cm_yield();continue;}
        if(h<0){cm_report("httpd accept error",h);status=1;break;}
        cm_print("httpd accepted\n");
        int failed=serve(h,hz);long e=cm_tcp_hangup(h);
        if(e<0)cm_report("httpd close error",e);
        cm_report("httpd handled",++handled);
        if(limit && handled>=limit){status=failed || e<0;break;}
    }
    long e=cm_tcp_hangup(listener);if(e<0)status=1;
    cm_print("httpd stopped: listener closed\n");return status;
}
