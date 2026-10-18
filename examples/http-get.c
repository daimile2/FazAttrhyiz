#include "cm_resolve.h"
/* HTTP/1.0 preview client. No libc, DNS, TLS, redirects or chunked decoding. */
#define BODY_PREVIEW 1024u
#define BODY_LIMIT (1024u*1024u)
#define HEADER_LIMIT 8192u
struct response {
    char line[256],text[127];
    unsigned used,headers,body,shown,text_used,length;
    int state,code,has_length;
};
static int lower(int c){return c>='A' && c<='Z'?c+('a'-'A'):c;}
static int field(const char *s,unsigned n,const char *name)
{
    unsigned i=0;while(name[i]){if(i>=n || lower(s[i])!=name[i])return 0;i++;}
    return i==n;
}
static int line_done(struct response *r)
{
    char *s=r->line;unsigned n=r->used;
    if(!n || s[n-1]!='\r')return -1;
    s[--n]=0;r->used=0;
    if(!r->state) {
        if(n<12 || s[0]!='H' || s[1]!='T' || s[2]!='T' || s[3]!='P' ||
           s[4]!='/' || s[5]!='1' || s[6]!='.' || (s[7]!='0' && s[7]!='1') ||
           s[8]!=' ' || s[9]<'2' || s[9]>'5' || s[10]<'0' || s[10]>'9' ||
           s[11]<'0' || s[11]>'9' || (n>12 && s[12]!=' '))return -1;
        r->code=(s[9]-'0')*100+(s[10]-'0')*10+s[11]-'0';
        char out[280];unsigned j=0;const char *prefix="http-get status: ";
        while(*prefix)out[j++]=*prefix++;
        for(unsigned i=0;i<n;i++)out[j++]=s[i];
        out[j++]='\n';out[j]=0;if(cm_print(out)<0)return -1;
        r->state=1;return 0;
    }
    if(!n){r->state=2;return cm_print("http-get body preview:\n")<0?-1:0;}
    unsigned colon=0;while(colon<n && s[colon]!=':')colon++;
    if(!colon || colon==n)return -1;
    /* HTTP/1.0 request should not elicit transfer-coded data. Reject rather
     * than presenting chunk metadata as decoded body. */
    if(field(s,colon,"transfer-encoding"))return -1;
    if(field(s,colon,"content-length")) {
        unsigned i=colon+1,length=0,digits=0;
        while(i<n && (s[i]==' ' || s[i]=='\t'))i++;
        while(i<n && s[i]>='0' && s[i]<='9') {
            unsigned digit=(unsigned)(s[i++]-'0');
            if(length>(BODY_LIMIT-digit)/10)return -1;
            length=length*10+digit;digits++;
        }
        while(i<n && (s[i]==' ' || s[i]=='\t'))i++;
        if(!digits || i!=n || (r->has_length && r->length!=length))return -1;
        r->has_length=1;r->length=length;
    }
    return 0;
}
static int flush(struct response *r)
{
    if(r->text_used && cm_output(r->text,r->text_used)<0)return -1;
    r->text_used=0;return 0;
}
static int consume(struct response *r,const char *p,unsigned n)
{
    for(unsigned i=0;i<n;i++) {
        unsigned char c=(unsigned char)p[i];
        if(r->state<2) {
            if(++r->headers>HEADER_LIMIT)return -1;
            if(c=='\n'){if(line_done(r))return -1;}
            else {
                if((c<32 && c!='\r' && c!='\t') || c>126 || r->used>=sizeof(r->line)-1)return -1;
                r->line[r->used++]=(char)c;
            }
        } else {
            if(r->body==BODY_LIMIT || (r->has_length && r->body>=r->length))return -1;
            r->body++;
            if(r->shown<BODY_PREVIEW) {
                if((c<32 && c!='\n' && c!='\t') || c>126)c='.';
                r->text[r->text_used++]=(char)c;r->shown++;
                if(r->text_used==sizeof(r->text) && flush(r))return -1;
            }
        }
    }
    return flush(r);
}
int main(int argc,char **argv,char **envp)
{
    (void)envp;uint32_t ip;uint16_t port;
    if((argc!=3 && argc!=4) || !cm_parse_port(argv[2],&port)) {
        cm_print("usage: http-get.elf HOST PORT [/path]\n");return 2;
    }
    int resolved=cm_resolve_ipv4(argv[1],&ip);
    if(resolved){cm_print(cm_hosts_error(resolved));return 2;}
    const char *path=argc==4?argv[3]:"/";unsigned pathlen=0;
    if(path[0]!='/'){cm_print("http-get: path must start with /\n");return 2;}
    while(path[pathlen]) {
        unsigned char c=(unsigned char)path[pathlen++];
        if(c<=32 || c>126 || c=='#' || pathlen>255){cm_print("http-get: invalid request path\n");return 2;}
    }
    uint64_t hz=cm_hz();if(!hz){cm_print("http-get: clock unavailable\n");return 1;}
    char request[512];unsigned size=0;
    const char *parts[]={"GET ",path," HTTP/1.0\r\nHost: ",argv[1],":",argv[2],"\r\nConnection: close\r\n\r\n"};
    for(unsigned i=0;i<sizeof(parts)/sizeof(parts[0]);i++) {
        const char *s=parts[i];while(*s){if(size==sizeof(request))return 2;request[size++]=*s++;}
    }
    long h=cm_tcp_dial(ip,port);if(h<0){cm_report("http-get connect error",h);return 1;}
    int status=0;long e=cm_tcp_write(h,request,size);
    if(e<0){cm_report("http-get send error",e);status=1;}
    struct response response={0};uint64_t start=cm_now(),last=start;char packet[256];
    while(!status) {
        uint64_t now=cm_now();
        if(now-start>=hz*10 || now-last>=hz*3){cm_print("http-get receive timeout\n");status=124;break;}
        long n=cm_tcp_read(h,packet,sizeof(packet));
        if(n>0) {
            last=cm_now();
            if(consume(&response,packet,(unsigned)n)){cm_print("\nhttp-get invalid/unsupported response or size limit\n");status=1;}
        } else if(!n) {
            if(response.state!=2 || (response.has_length && response.body!=response.length)) {
                cm_print("\nhttp-get truncated response\n");status=1;
            } else if(response.code>=300)status=4;
            break;
        } else if(n==CM_EAGAIN)cm_yield();
        else {cm_report("http-get recv error",n);status=1;}
    }
    cm_print("\n");cm_report("http-get body bytes",response.body);cm_report("http-get shown bytes",response.shown);
    e=cm_tcp_hangup(h);if(e<0){cm_report("http-get close error",e);if(!status)status=1;}
    cm_report("http-get exit",status);return status;
}
