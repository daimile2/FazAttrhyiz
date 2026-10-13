/* Tiny freestanding main, no libc, TLS or dynamic interpreter. */
typedef unsigned long u64;
static long write_text(const char *p)
{
    u64 n=0;while(p[n])n++;
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"(2L),"D"(1L),"S"(p),"d"(n):"rcx","r11","memory");
    return result;
}
static const char ok[]="argv/envp crt main verified\n";
static long print_item(const char *kind,unsigned index,const char *text)
{
    char line[128];unsigned n=0;
    while(*kind)line[n++]=*kind++;
    line[n++]='[';if(index>=10)line[n++]=(char)('0'+index/10);
    line[n++]=(char)('0'+index%10);line[n++]=']';line[n++]='=';
    while(*text && n<sizeof(line)-1)line[n++]=*text++;
    line[n]=0;return write_text(line);
}
/* Includes initialized data so the existing fixture linker maps rodata as data. */
volatile unsigned long argv_report[4]={0,0,0,0x434d};
int main(int argc,char **argv,char **envp)
{
    unsigned short cs;__asm__ volatile("mov %%cs,%0":"=r"(cs));
    if((cs&3)!=3 || argc<1 || !argv || !argv[0] || argv[argc] || !envp)return 1;
    argv_report[0]=(unsigned)argc;argv_report[1]=cs&3;
    for(int i=0;i<argc;i++) {
        if(!argv[i] || print_item("argv",(unsigned)i,argv[i])<0)return 2;
    }
    unsigned envc=0;
    while(envp[envc]) {
        if(envc==32 || print_item("envp",envc,envp[envc])<0)return 3;
        envc++;
    }
    argv_report[2]=envc;argv_report[3]=1;
    return write_text(ok)<0?4:0;
}
