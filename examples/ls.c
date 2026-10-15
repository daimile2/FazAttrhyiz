#include "capmix.h"
int main(int argc,char **argv,char **envp)
{
    (void)envp;if(argc>2){cm_print("usage: ls.elf [/absolute/directory]\n");return 2;}
    const char *path=argc==2?argv[1]:"/";
    for(unsigned i=0;i<256;i++) {
        cm_sdk_dirent_t item;long e=cm_call(CM_READDIR,(uintptr_t)path,i,(uintptr_t)&item,0);
        if(e==CM_ENOENT){cm_report("ls entries",i);return 0;}
        if(e<0){cm_report("ls error",e);return 1;}
        char line[61];unsigned n=0;while(n<58 && item.name[n]){line[n]=item.name[n];n++;}
        if(item.kind==2)line[n++]='/';
        line[n++]='\n';line[n]=0;
        if(cm_print(line)<0)return 1;
    }
    cm_print("ls: directory entry limit reached\n");return 3;
}
