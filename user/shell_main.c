#include "runtime.h"
#define JOBS 8
static cm_user_child_t jobs[JOBS];
static char line[256];static unsigned length;static int overflow;
static void error(long e){uputs("shell error=");unum(e);uputs("\n");}
static void finished(const cm_user_wait_t *r)
{uputs("shell child done: pid=");unum(r->pid);uputs(" exit=");unum(r->exit_code);uputs(" reap=ok\n");}
static void reap(void)
{
    for(unsigned i=0;i<JOBS;i++)if(jobs[i].handle>0){cm_user_wait_t r;long e=uwait(jobs[i].handle,&r,CM_WAIT_NONBLOCK);
        if(!e){jobs[i].handle=0;finished(&r);}else if(e!=CM_ERR_AGAIN)error(e);}
}
static unsigned pending(void)
{unsigned n=0;for(unsigned i=0;i<JOBS;i++)n+=jobs[i].handle>0;return n;}
static uint64_t now(void){return (uint64_t)ucall(CM_SYS_CLOCK,0,0,0,0);}
static int wait_bounded(unsigned seconds)
{
    long hz=ucall(CM_SYS_TSC_HZ,0,0,0,0);
    uint64_t start=now(),limit=hz>0?(uint64_t)hz*seconds:0;
    /* A bounded iteration fallback also handles an unavailable calibration. */
    for(unsigned steps=0;steps<2000000;steps++) {
        reap();if(!pending())return 1;
        if(limit && now()-start>=limit)break;
        uyield();
    }
    return !pending();
}
static int drain(void)
{
    reap();if(!pending())return 1;
    uputs("shell shutdown: grace=5s waiting background\n");
    if(wait_bounded(5))return 1;
    uputs("shell shutdown: timeout; kill background; kill-wait=2s\n");
    for(unsigned i=0;i<JOBS;i++)if(jobs[i].handle>0){long e=ucall(CM_SYS_CHILD_KILL,jobs[i].handle,0,0,0);if(e)error(e);}
    if(wait_bounded(2)){uputs("shell shutdown: killed and reaped background\n");return 1;}
    uputs("shell shutdown: deadline exhausted; live kernel continuation retained\n");return 0;
}
static long kill_job(const char *text)
{
    uint64_t pid=0;if(!*text)return CM_ERR_INVAL;
    while(*text>='0' && *text<='9'){if(pid>(UINT64_MAX-9)/10)return CM_ERR_INVAL;pid=pid*10+(unsigned)(*text++-'0');}
    while(*text==' ' || *text=='\t')text++;
    if(*text || !pid)return CM_ERR_INVAL;
    for(unsigned i=0;i<JOBS;i++)if(jobs[i].handle>0 && jobs[i].pid==pid){
        long e=ucall(CM_SYS_CHILD_KILL,jobs[i].handle,0,0,0);
        if(!e){uputs("shell kill accepted: pid=");unum(pid);uputs("\n");}return e;
    }
    return CM_ERR_NOENT;
}
static long run(char *text)
{
    const char *args[34];unsigned n=0;while(*text){while(*text==' ' || *text=='\t')text++;if(!*text)break;
        if(n==33)return CM_ERR_NOSPC;
        args[n++]=text;while(*text && *text!=' ' && *text!='\t')text++;if(*text)*text++=0;}
    int interactive=n && ueq(args[0],"-i");
    if(interactive){for(unsigned i=1;i<n;i++)args[i-1]=args[i];n--;}
    int bg=n && ueq(args[n-1],"&");if(bg)n--;
    if(interactive && bg)return CM_ERR_INVAL;
    if(!n || n>32 || args[0][0]!='/')return CM_ERR_INVAL;
    args[n]=NULL;reap();unsigned slot=0;if(bg){while(slot<JOBS && jobs[slot].handle)slot++;if(slot==JOBS)return CM_ERR_NOSPC;}
    const char *env[]={"PATH=/bin","CAPMIX=product",NULL};cm_user_child_t child;long e=uspawn(args[0],args,env,interactive?CM_SPAWN_CONSOLE:0,&child);if(e)return e;
    uputs(bg?"shell background start: pid=":"shell run: pid=");unum(child.pid);uputs(" cpu=");unum(child.cpu);uputs("\n");
    if(bg){jobs[slot]=child;return 0;}
    cm_user_wait_t result;while((e=uwait(child.handle,&result,CM_WAIT_NONBLOCK))==CM_ERR_AGAIN){reap();uyield();}
    if(!e)finished(&result);
    return e;
}
static long list(const char *path)
{
    for(unsigned i=0;i<256;i++){cm_dirent_t entry;long e=ucall(35,(uintptr_t)path,i,(uintptr_t)&entry,0);
        if(e==CM_ERR_NOENT)return 0;
        if(e)return e;
        uputs(entry.name);if(entry.kind==CM_INO_DIR)uputs("/");uputs("\n");}return 0;
}
static long cat(const char *path)
{
    long fd=ucall(11,(uintptr_t)path,CM_O_RDONLY,0,0);if(fd<0)return fd;unsigned total=0;long e=0;
    char data[129];while(total<4096){long n=ucall(12,fd,(uintptr_t)data,128,0);if(n<0){e=n;break;}if(!n)break;
        for(long i=0;i<n;i++){unsigned char c=(unsigned char)data[i];if(c<32 && c!='\n' && c!='\t')data[i]='.';if(c>126)data[i]='.';}
        data[n]=0;uputs(data);total+=(unsigned)n;}
    (void)ucall(13,fd,0,0,0);uputs("\n");return e;
}
static long command(char *text)
{
    while(*text==' ' || *text=='\t')text++;
    if(!*text)return 0;
    char *args=text;while(*args && *args!=' ' && *args!='\t')args++;if(*args)*args++=0;while(*args==' ' || *args=='\t')args++;
    if(ueq(text,"help")){if(*args)return CM_ERR_INVAL;uputs("help | echo TEXT | ls [PATH] | cat PATH | run [-i] ELF [ARGS] [&] | kill PID | exit | poweroff/shutdown\n");return 0;}
    if(ueq(text,"echo")){uputs(args);uputs("\n");return 0;}
    if(ueq(text,"run"))return run(args);
    if(ueq(text,"kill"))return kill_job(args);
    if(ueq(text,"ls"))return list(*args?args:"/");
    if(ueq(text,"cat"))return *args=='/'?cat(args):CM_ERR_INVAL;
    if(ueq(text,"exit")){if(*args)return CM_ERR_INVAL;return drain()?1:CM_ERR_BUSY;}
    if(ueq(text,"poweroff") || ueq(text,"shutdown")){if(*args)return CM_ERR_INVAL;int clean=drain();return ucall(37,uboot,clean?0:CM_POWEROFF_FORCE,0,0);}
    return CM_ERR_NOTSUP;
}
int main(int argc,char **argv,char **envp)
{
    (void)argc;(void)argv;(void)envp;
    /* A plain run of this management ELF receives no console bundle. */
    if(ucall(CM_SYS_CONSOLE_WRITE,uconsole,0,0,0)<0)return 1;
    uputs("shell ELF ready: CPL=");unum(ucpl());uputs(" PS/2 -> user shell -> FB+serial\ncapmix> ");
    for(;;){reap();char ch;long e=ucall(31,uconsole,(uintptr_t)&ch,1,0);
        if(e==CM_ERR_AGAIN){uyield();continue;}if(e){error(e);uyield();continue;}
        if(ch=='\r')ch='\n';
        if(ch=='\b'){if(length && !overflow){length--;uputs("\b \b");}continue;}
        if(ch=='\n'){uputs("\n");line[length]=0;e=overflow?CM_ERR_NOSPC:command(line);length=0;overflow=0;
            if(e==1){uputs("shell user exit: background drained\n");return 0;}if(e<0)error(e);uputs("capmix> ");continue;}
        if(ch=='\t')ch=' ';
        if((unsigned char)ch<32 || (unsigned char)ch>126)continue;
        if(length+1>=sizeof(line))overflow=1;
        if(!overflow)line[length++]=ch;
        char echo[2]={ch,0};uputs(echo);
    }
}
