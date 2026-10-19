#include "capmix.h"
int main(int argc,char **argv,char **envp)
{
    (void)envp;
    if(argc>2){cm_print_fd(2,"usage: cat.faz [/absolute/file] (default stdin, raw bytes)\n");return 2;}
    long fd=argc==2?cm_open(argv[1]):0;if(fd<0){cm_report("cat open error",fd);return 1;}
    int status=0;char buffer[127];
    for(;;){
        long n=cm_read(fd,buffer,sizeof(buffer));
        if(n<0){cm_report("cat read error",n);status=1;break;}if(!n)break;
        if(cm_output(buffer,(size_t)n)<0){status=1;break;}}
    if(argc==2 && cm_close(fd)<0)status=1;
    return status;
}
