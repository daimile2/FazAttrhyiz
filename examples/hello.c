#include "capmix.h"
/* No Linux/glibc dependencies. main receives the initial stack vectors. */
int main(int argc,char **argv,char **envp)
{
    (void)envp;
    if(argc<1 || !argv || !argv[0] || argv[argc])return 1;
    if(cm_print("hello from external gcc static ELF\n")<0)return 2;
    for(int i=0;i<argc;i++) {
        char line[128];unsigned n=0;
        const char *prefix="argv[";while(*prefix)line[n++]=*prefix++;
        if(i>=10)line[n++]=(char)('0'+i/10);
        line[n++]=(char)('0'+i%10);line[n++]=']';line[n++]='=';
        const char *arg=argv[i];while(*arg && n<sizeof(line)-2)line[n++]=*arg++;
        line[n++]='\n';line[n]=0;
        if(cm_print(line)<0)return 3;
    }
    return 0;
}
