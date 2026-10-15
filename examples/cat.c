#include "capmix.h"
int main(int argc,char **argv,char **envp)
{
    (void)envp;
    if(argc!=2){cm_print("usage: cat.elf /absolute/file (text, <=1MiB)\n");return 2;}
    long fd=cm_open(argv[1]);if(fd<0){cm_report("cat open error",fd);return 1;}
    unsigned total=0;int status=0;char buffer[127];
    while(total<1024*1024){unsigned left=1024*1024-total;
        long n=cm_read(fd,buffer,left<sizeof(buffer)?left:sizeof(buffer));
        if(n<0){cm_report("cat read error",n);status=1;break;}if(!n)break;
        for(long i=0;i<n;i++){unsigned char c=(unsigned char)buffer[i];if((c<32 && c!='\n' && c!='\t') || c>126)buffer[i]='.';}
        if(cm_output(buffer,(size_t)n)<0){status=1;break;}total+=(unsigned)n;}
    if(total==1024*1024){cm_print("cat: display limit reached\n");status=3;}
    if(cm_close(fd)<0)status=1;
    cm_report("cat bytes read",total);return status;
}
