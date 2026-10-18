/* Native userspace benchmark verifier, deliberately outside timed regions. */
#include "capmix.h"
int main(int argc,char **argv,char **envp)
{
    (void)envp;if(argc!=3)return 2;
    unsigned expected=0;
    for(const char *p=argv[2];*p;p++) {
        if(*p<'0'||*p>'9'||expected>104857)return 2;
        expected=expected*10+(unsigned)(*p-'0');
    }
    if(!expected || expected>1048576)return 2;
    long fd=cm_open(argv[1]);if(fd<0)return 3;
    char buffer[4096];unsigned total=0;int status=0;
    for(;;) {
        long n=cm_read(fd,buffer,sizeof(buffer));
        if(n<0 || n>(long)sizeof(buffer)){status=4;break;}
        if(!n)break;
        if((unsigned)n>expected-total){status=5;break;}
        for(long i=0;i<n;i++)if(buffer[i]!='x'){status=6;break;}
        if(status)break;
        total+=(unsigned)n;
    }
    if(cm_close(fd)<0)status=7;
    if(total!=expected)status=8;
    if(!status)cm_report("perf pipe verified bytes",total);
    return status;
}
