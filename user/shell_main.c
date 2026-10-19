#include "runtime.h"
#define JOBS 8
static cm_user_child_t jobs[JOBS];
static char line[256];static unsigned length;static int overflow;
static int input_fd=-1,output_fd=-1;
/* v1 console records: one bounded capability write per complete record.
 * Immediate input echo stays unchanged; scripts synchronize on sequence IDs. */
static uint64_t sequence=1;
static char *append_text(char *p,const char *s){while(*s)*p++=*s++;return p;}
static char *append_number(char *p,int64_t n)
{
    char digits[24];unsigned used=0;uint64_t v=(uint64_t)n;
    if(n<0){*p++='-';v=0-v;}
    do{digits[used++]=(char)('0'+v%10);v/=10;}while(v);
    while(used)*p++=digits[--used];
    return p;
}
static void record(const char *event,long status)
{
    char b[112],*p=append_text(b,"\n[SHELL v=1 event=");p=append_text(p,event);
    p=append_text(p," seq=");p=append_number(p,(int64_t)sequence);
    p=append_text(p," status=");p=append_number(p,status);p=append_text(p,"]\n");*p=0;uputs(b);
}
static void prompt(void){record("ready",0);uputs("fazattrhyiz> ");}
static long out_bytes(const char *s,unsigned n)
{
    if(output_fd<0){uputs(s);return 0;}
    while(n){long e=ucall(CM_SYS_WRITE,output_fd,(uintptr_t)s,n,0);
        if(e<0)return e;
        if(!e)return CM_ERR_IO;
        s+=e;n-=(unsigned)e;}
    return 0;
}
static long out(const char *s){return out_bytes(s,ulen(s));}
static void error(long e){char b[64],*p=append_text(b,"shell error=");p=append_number(p,e);*p++='\n';*p=0;uputs(b);}
static void finished(const cm_user_wait_t *r)
{char b[112],*p=append_text(b,"shell child done: pid=");p=append_number(p,r->pid);p=append_text(p," exit=");p=append_number(p,r->exit_code);p=append_text(p," reap=ok\n");*p=0;uputs(b);}
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
    const char *env[]={"PATH=/bin","CAPMIX=product",NULL};cm_user_child_t child;long e=uspawn_stdio(args[0],args,env,interactive?CM_SPAWN_CONSOLE:0,&child,input_fd,output_fd);if(e)return e;
    uputs(bg?"shell background start: pid=":"shell run: pid=");unum(child.pid);uputs(" cpu=");unum(child.cpu);uputs("\n");
    if(bg){jobs[slot]=child;return 0;}
    cm_user_wait_t result;while((e=uwait(child.handle,&result,CM_WAIT_NONBLOCK))==CM_ERR_AGAIN){reap();uyield();}
    if(!e){finished(&result);if((input_fd>=0 || output_fd>=0) && result.exit_code)e=CM_ERR_IO;}
    return e;
}
/* Fixed /bin search, no environment PATH expansion or implicit console grant. */
static int short_name(const char *name)
{
    if(ueq(name,"help") || ueq(name,"kill") || ueq(name,"exit") ||
       ueq(name,"poweroff") || ueq(name,"shutdown"))return 0;
    unsigned n=0;
    for(;name[n];n++)if(n>=63 || !((name[n]>='a' && name[n]<='z') ||
       (name[n]>='A' && name[n]<='Z') || (name[n]>='0' && name[n]<='9') ||
       name[n]=='-' || name[n]=='_'))return 0;
    return n!=0;
}
static long short_run(const char *name,const char *args)
{
    if(ueq(name,"nc")) {
        uputs("nc needs keyboard access: use run -i /bin/nc.faz HOST PORT\n");
        return CM_ERR_NOTSUP;
    }
    if(!short_name(name))return CM_ERR_NOTSUP;
    if(ulen(name)+ulen(args)+11>=384)return CM_ERR_NOSPC;
    for(unsigned legacy=0;legacy<2;legacy++) {
        char buffer[384],*p=append_text(buffer,"/bin/");
        p=append_text(p,name);p=append_text(p,legacy?".elf":".faz");
        if(*args){*p++=' ';p=append_text(p,args);}*p=0;
        long e=run(buffer);
        if(e!=CM_ERR_NOENT || legacy)return e;
    }
    return CM_ERR_NOENT;
}
static long list(const char *path)
{
    for(unsigned i=0;i<256;i++){cm_dirent_t entry;long e=ucall(35,(uintptr_t)path,i,(uintptr_t)&entry,0);
        if(e==CM_ERR_NOENT)return 0;
        if(e)return e;
        e=out(entry.name);if(!e && entry.kind==CM_INO_DIR)e=out("/");if(!e)e=out("\n");if(e)return e;
    }
    return CM_ERR_NOSPC;
}
static long cat(const char *path)
{
    int own=*path!=0;long fd=own?ucall(11,(uintptr_t)path,CM_O_RDONLY,0,0):input_fd;
    if(fd<0)return own?fd:CM_ERR_INVAL;
    unsigned total=0;long e=0;char data[129];int raw=output_fd>=0;
    /* Redirected output is data, not a terminal preview: no escaping or limit. */
    while(raw || total<4096) {
        long n=ucall(12,fd,(uintptr_t)data,128,0);
        if(n<0){e=n;break;}if(!n)break;
        if(!raw)for(long i=0;i<n;i++) {
            unsigned char c=(unsigned char)data[i];
            if((c<32 && c!='\n' && c!='\t') || c>126)data[i]='.';
        }
        e=out_bytes(data,(unsigned)n);if(e)break;
        if(!raw)total+=(unsigned)n;
    }
    if(!e && !raw && total==4096) {
        long n=ucall(12,fd,(uintptr_t)data,1,0);
        if(n<0)e=n;
        else if(n){uputs("\ncat: terminal preview limited to 4096 bytes; redirect to copy raw data\n");e=CM_ERR_NOSPC;}
    }
    if(own){long close=ucall(13,fd,0,0,0);if(!e)e=close;}
    if(output_fd<0)uputs("\n");
    return e;
}
static long command(char *text)
{
    while(*text==' ' || *text=='\t')text++;
    if(!*text)return 0;
    char *args=text;while(*args && *args!=' ' && *args!='\t')args++;if(*args)*args++=0;while(*args==' ' || *args=='\t')args++;
    if(ueq(text,"help")){if(*args)return CM_ERR_INVAL;uputs("help | COMMAND [ARGS] [&] | > >> < (foreground) | echo TEXT | ls [PATH] | cat PATH | run [-i] ELF [ARGS] [&] | kill PID | exit | poweroff/shutdown\n");return 0;}
    if(ueq(text,"echo")){long e=out(args);return e?e:out("\n");}
    if(ueq(text,"run"))return run(args);
    if(ueq(text,"kill"))return kill_job(args);
    if(ueq(text,"ls"))return list(*args?args:"/");
    if(ueq(text,"cat"))return (!*args || *args=='/')?cat(args):CM_ERR_INVAL;
    if(ueq(text,"exit")){if(*args)return CM_ERR_INVAL;return drain()?1:CM_ERR_BUSY;}
    if(ueq(text,"poweroff") || ueq(text,"shutdown")){if(*args)return CM_ERR_INVAL;int clean=drain();return ucall(37,uboot,clean?0:CM_POWEROFF_FORCE,0,0);}
    return short_run(text,args);
}
/* No quotes/expansion or pipe emulation. Tokenize the complete command before
 * opening anything so unsupported grammar cannot truncate an output file. */
static long dispatch(char *text)
{
    int special=0;for(char *s=text;*s;s++)if(*s=='<' || *s=='>' || *s=='|')special=1;
    if(!special)return command(text);
    char storage[512],rebuilt[256];const char *token[66],*args[34];unsigned used=0,nt=0,na=0;
    for(const char *s=text;*s;) {
        if(*s==' ' || *s=='\t'){s++;continue;}
        if(nt==66 || used+3>=sizeof(storage))return CM_ERR_NOSPC;
        token[nt++]=storage+used;
        if(*s=='<' || *s=='>' || *s=='|') {
            char c=*s++;storage[used++]=c;if(c=='>' && *s=='>')storage[used++]=*s++;
        } else while(*s && *s!=' ' && *s!='\t' && *s!='<' && *s!='>' && *s!='|') {
            if(*s=='\"' || *s=='\'' || *s=='\\')return CM_ERR_NOTSUP;
            if(used+2>=sizeof(storage))return CM_ERR_NOSPC;
            storage[used++]=*s++;
        }
        storage[used++]=0;
    }
    const char *in=NULL,*output=NULL;int append=0;
    for(unsigned i=0;i<nt;i++) {
        const char *t=token[i];
        if(ueq(t,"|"))return CM_ERR_NOTSUP;
        if(ueq(t,"&"))return CM_ERR_INVAL;
        if(ueq(t,"<") || ueq(t,">") || ueq(t,">>")) {
            int read=ueq(t,"<");
            if(i+1==nt || token[i+1][0]!='/' || (read?in!=NULL:output!=NULL))return CM_ERR_INVAL;
            if(read)in=token[++i];else {append=ueq(t,">>");output=token[++i];}
        } else {if(na==33)return CM_ERR_NOSPC;args[na++]=t;}
    }
    if(!na || (in && output && ueq(in,output)))return CM_ERR_INVAL;
    if(!ueq(args[0],"echo") && !ueq(args[0],"ls") && !ueq(args[0],"cat") && !ueq(args[0],"run") && !short_name(args[0]))return CM_ERR_NOTSUP;
    if(ueq(args[0],"run") && (na<2 || args[1][0]!='/'))return CM_ERR_INVAL;
    if((ueq(args[0],"ls") || ueq(args[0],"cat")) &&
       (na>2 || (na==2 && args[1][0]!='/')))return CM_ERR_INVAL;
    if(ueq(args[0],"cat") && na==1 && !in)return CM_ERR_INVAL;
    unsigned n=0;for(unsigned i=0;i<na;i++) {
        const char *s=args[i];if(i)rebuilt[n++]=' ';
        while(*s){if(n+1>=sizeof(rebuilt))return CM_ERR_NOSPC;rebuilt[n++]=*s++;}
    }
    rebuilt[n]=0;long e=0;
    if(in){input_fd=(int)ucall(11,(uintptr_t)in,CM_O_RDONLY,0,0);if(input_fd<0){e=input_fd;input_fd=-1;return e;}}
    if(output){output_fd=(int)ucall(11,(uintptr_t)output,CM_O_WRONLY|CM_O_CREAT|(append?CM_O_APPEND:CM_O_TRUNC),0,0);
        if(output_fd<0){e=output_fd;output_fd=-1;goto close;}}
    e=command(rebuilt);
    if(output_fd>=0){long sync=ucall(16,output_fd,0,0,0);if(!e)e=sync;}
close:
    if(input_fd>=0){long c=ucall(13,input_fd,0,0,0);if(!e)e=c;}
    if(output_fd>=0){long c=ucall(13,output_fd,0,0,0);if(!e)e=c;}
    input_fd=output_fd=-1;
    uputs(e?"shell redirect failed: status=":"shell redirect committed: status=");unum(e);uputs("\n");
    return e;
}
int main(int argc,char **argv,char **envp)
{
    (void)argc;(void)argv;(void)envp;
    /* A plain run of this management ELF receives no console bundle. */
    if(ucall(CM_SYS_CONSOLE_WRITE,uconsole,0,0,0)<0)return 1;
    uputs("shell ELF ready: CPL=");unum(ucpl());uputs(" PS/2 -> user shell -> FB+serial\n");prompt();
    for(;;){reap();char ch;long e=ucall(31,uconsole,(uintptr_t)&ch,1,0);
        if(e==CM_ERR_AGAIN){uyield();continue;}if(e){error(e);uyield();continue;}
        if(ch=='\r')ch='\n';
        if(ch=='\b'){if(length && !overflow){length--;uputs("\b \b");}continue;}
        if(ch=='\n'){record("begin",0);line[length]=0;e=overflow?CM_ERR_NOSPC:dispatch(line);length=0;overflow=0;
            record("end",e);
            if(e==1){uputs("shell user exit: background drained\n");return 0;}if(e<0)error(e);sequence++;prompt();continue;}
        if(ch=='\t')ch=' ';
        if((unsigned char)ch<32 || (unsigned char)ch>126)continue;
        if(length+1>=sizeof(line))overflow=1;
        if(!overflow)line[length++]=ch;
        char echo[2]={ch,0};uputs(echo);
    }
}
