#include <capmix/klog.h>
#include "../sdk/include/cm_hosts.h"
typedef struct {const char *s;unsigned pos,chunk;long error;} fixture;
static long reader(void *ctx,void *p,size_t cap)
{
    fixture *f=ctx;if(f->error)return f->error;
    char *b=p;unsigned n=0;
    while(n<cap && n<f->chunk && f->s[f->pos])b[n++]=f->s[f->pos++];
    return n;
}
int cm_hosts_selftest(void)
{
    int fails=0;uint32_t ip=0;
    const char *s="# comment\r\n::1 ignored\n999.0.0.1 bad\n10.0.2.2 gateway Host.Test # aliases\r\n127.0.0.1 localhost";
    for(unsigned chunk=1;chunk<=128;chunk*=2){fixture f={s,0,chunk,0};
        if(cm_hosts_resolve("HOST.test",&ip,reader,&f) || ip!=0x0a000202)fails++;
        f.pos=0;if(cm_hosts_resolve("localhost",&ip,reader,&f) || ip!=0x7f000001)fails++;
        f.pos=0;if(cm_hosts_resolve("10.1.2.3",&ip,reader,&f)||ip!=0x0a010203)fails++;
        f.pos=0;if(cm_hosts_resolve("missing.invalid",&ip,reader,&f)!=CM_HOSTS_NOTFOUND)fails++;
    }
    if(cm_hosts_resolve("10.0.2.2",&ip,0,0)||ip!=0x0a000202)fails++;
    if(cm_hosts_resolve("bad\r\nHost",&ip,0,0)!=CM_HOSTS_INVALID)fails++;
    if(cm_hosts_resolve("256.1.2.3",&ip,0,0)!=CM_HOSTS_NOTFOUND)fails++;
    fixture err={"",0,1,-1};if(cm_hosts_resolve("10.0.2.2",&ip,reader,&err)!=CM_HOSTS_IO)fails++;
    static char big[CM_HOSTS_FILE_MAX+2];
    for(unsigned i=0;i<sizeof(big)-1;i++)big[i]=' ';
    big[sizeof(big)-1]=0;fixture f={big,0,128,0};
    if(cm_hosts_resolve("missing",&ip,reader,&f)!=CM_HOSTS_LIMIT)fails++;
    for(unsigned i=0;i<sizeof(big)-1;i++)big[i]='\n';
    f.pos=0;
    if(cm_hosts_resolve("10.0.2.2",&ip,reader,&f)!=CM_HOSTS_LIMIT)fails++;
    kinfo("hosts stub: aliases/case/CRLF/chunks/literal/missing/limits/I-O %s (%d failures); DNS network=disabled",fails?"FAIL":"PASS",fails);
    return fails;
}
