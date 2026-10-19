/* Native end-to-end byte comparison; no dependence on terminal rendering. */
#include "capmix.h"
static unsigned char source[1024],copy[1024];
int main(int argc,char **argv,char **envp)
{
    (void)envp;if(argc<3)return 2;
    for(int i=2;i<argc;i++) {
        long a=cm_open(argv[1]),b=cm_open(argv[i]);
        if(a<0 || b<0){if(a>=0)cm_close(a);if(b>=0)cm_close(b);return 3;}
        unsigned total=0,nul=0,high=0;int error=0;
        for(;;) {
            long n=cm_read(a,source,sizeof(source)),m=cm_read(b,copy,sizeof(copy));
            if(n<0 || m!=n){error=4;break;}if(!n)break;
            for(long j=0;j<n;j++) {
                if(source[j]!=copy[j]){error=5;break;}
                nul|=source[j]==0;high|=source[j]>=128;
            }
            if(error)break;
            total+=(unsigned)n;
        }
        if(cm_close(a)<0 || cm_close(b)<0)error=6;
        if(error)return error;
        if(total<=4096 || !nul || !high)return 7;
        cm_report("copy integrity verified bytes",total);
    }
    return 0;
}
