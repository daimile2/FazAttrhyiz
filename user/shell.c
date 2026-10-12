#include "capmix/power.h"
#include "capmix/shell.h"
#include "capmix/boot.h"
#include "capmix/kernel.h"
#include "capmix/console.h"
#include "capmix/fs.h"
#include <string.h>
#define LINE_MAX 256u
static char line[LINE_MAX];
static unsigned length;
static int overflow;
static int equal(const char *a,const char *b) {size_t n=strlen(a);return n==strlen(b) && !memcmp(a,b,n);}
static int path_ok(const char *p)
{
    if(*p!='/')return 0;
    for(;*p;p++)if(*p==' ' || *p=='\t')return 0;
    return 1;
}
static cm_error_t list(const char *path)
{
    cm_stat_t st;cm_error_t e=cm_vfs_stat(path,&st);if(e)return e;
    if(st.kind!=CM_INO_DIR)return CM_ERR_NOTDIR;
    for(unsigned i=0;i<256;i++) {
        cm_dirent_t entry;e=cm_vfs_readdir(path,i,&entry);
        if(e==CM_ERR_NOENT)return CM_OK;
        if(e)return e;
        cm_puts(entry.name);if(entry.kind==CM_INO_DIR)cm_putchar('/');cm_putchar('\n');
    }
    return CM_OK;
}
static cm_error_t cat(const char *path)
{
    cm_file_t *file=NULL;cm_error_t e=cm_vfs_open(path,CM_O_RDONLY,&file);if(e)return e;
    unsigned total=0;char data[128];
    while(total<4096) {
        int64_t n=cm_vfs_read(file,data,sizeof(data));
        if(n<0){e=(cm_error_t)n;break;}if(!n)break;
        for(int64_t i=0;i<n;i++) {
            unsigned char c=(unsigned char)data[i];
            cm_putchar((c>=32 && c<=126) || c=='\n' || c=='\t' ? (char)c : '.');
        }
        total+=(unsigned)n;
    }
    cm_vfs_close(file);cm_putchar('\n');
    if(total==4096)cm_puts("shell: cat limit 4096 bytes\n");
    return e;
}
static cm_error_t run(const char *path)
{
    cm_spawn_attr_t attr={.path=path,.qos=CM_QOS_USER_INTERACTIVE};cm_id_t pid=0;
    cm_error_t e=cm_posix_spawn(&attr,&pid);if(e)return e;
    cm_proc_t *p=cm_proc_lookup(pid);
    kinfo("shell run: pid=%llu path=%s",(unsigned long)pid,path);
    /* Native PID1 yields; never nest the scheduler's host context. */
    while(!p->exited || (p->main && p->main->id==p->elf_thread_id))cm_sched_yield();
    int64_t code=p->exit_code;uint64_t preempts=p->elf_preempts;
    e=cm_proc_release_product_elf(pid);
    kinfo("shell run done: pid=%llu exit=%lld preempts=%llu reclaim=%d",
          (unsigned long)pid,(long)code,(unsigned long)preempts,e);
    return e;
}
/* Positive one requests shell exit; all command errors are negative. */
static cm_error_t command(char *text)
{
    while(*text==' ' || *text=='\t')text++;
    if(!*text)return CM_OK;
    char *args=text;while(*args && *args!=' ' && *args!='\t')args++;
    if(*args)*args++=0;
    while(*args==' ' || *args=='\t')args++;
    if(equal(text,"echo")){cm_puts(args);cm_putchar('\n');return CM_OK;}
    if(equal(text,"help")) {
        if(*args)return CM_ERR_INVAL;
        cm_puts("help | echo TEXT | ls [PATH] | cat PATH | run ELF_PATH | exit | poweroff/shutdown\n");return CM_OK;
    }
    if(equal(text,"poweroff") || equal(text,"shutdown")) {
        if(*args)return CM_ERR_INVAL;
        cm_error_t e=cm_vfs_sync();
        if(e){kerr("poweroff: root sync failed=%d; staying in shell",e);return e;}
        kinfo("poweroff: root sync=0");
        cm_poweroff();return CM_OK;
    }
    if(equal(text,"exit"))return *args?CM_ERR_INVAL:1;
    if(equal(text,"ls")){if(!*args)args="/";return path_ok(args)?list(args):CM_ERR_INVAL;}
    if(equal(text,"cat"))return path_ok(args)?cat(args):CM_ERR_INVAL;
    if(equal(text,"run"))return path_ok(args)?run(args):CM_ERR_INVAL;
    return CM_ERR_NOTSUP;
}
static int feed(char c)
{
    if(c=='\r')c='\n';
    if(c=='\b') {
        if(length && !overflow){length--;cm_puts("\b \b");}
        return 0;
    }
    if(c=='\n') {
        cm_putchar('\n');line[length]=0;
        cm_error_t e=overflow?CM_ERR_NOSPC:command(line);
        length=0;overflow=0;
        if(e==1)return 1;
        if(e<0)kinfo("shell error: %d (help lists commands)",e);
        cm_puts("capmix> ");return 0;
    }
    if(c=='\t')c=' ';
    if((unsigned char)c<32 || (unsigned char)c>126)return 0;
    if(length+1>=sizeof(line))overflow=1;
    if(!overflow)line[length++]=c;
    cm_putchar(c);return 0;
}
void cm_shell_run(void)
{
    length=0;overflow=0;
    kinfo("shell ready: PID1 native controller, PS/2 input, FB + serial; run enters CPL=3");
    cm_puts("CapMix shell (help for commands)\ncapmix> ");
    for(;;) {
        char c;cm_error_t e=cm_ps2_read_char(&c);
        if(e==CM_OK){if(feed(c))break;}
        else if(e!=CM_ERR_AGAIN)kerr("shell input error=%d",e);
        cm_sched_yield();
    }
    cm_error_t e=cm_vfs_sync();kinfo("shell exit: root sync=%d",e);
}
int cm_shell_selftest(void)
{
    char help[]="help",empty[]="  ",bad[]="unknown",relative[]="run relative",missing[]="run /__m5_missing__.elf";
    char offargs[]="poweroff no", shutdownargs[]="shutdown no";
    if(command(offargs)!=CM_ERR_INVAL || command(shutdownargs)!=CM_ERR_INVAL)return 1;
    char exit[]="exit",exitarg[]="exit no",echo[]="echo m5 parser verified";
    if(command(help) || command(empty) || command(echo) || command(bad)!=CM_ERR_NOTSUP ||
       command(relative)!=CM_ERR_INVAL || command(missing)!=CM_ERR_NOENT || command(exit)!=1 ||
       command(exitarg)!=CM_ERR_INVAL)return 1;
    if(!cm_boot_parse_selftest("capmix.mode=selftest") || cm_boot_parse_selftest("capmix.mode=selftestBAD") ||
       cm_boot_parse_selftest("capmix.mode=selftest capmix.mode=product") ||
       !cm_boot_parse_selftest("capmix.mode=product capmix.mode=selftest"))return 1;
    length=0;overflow=0;
    for(unsigned i=0;i<LINE_MAX+1;i++)feed('x');
    if(!overflow || length!=LINE_MAX-1)return 1;
    feed('\n');feed('a');feed('\b');
    if(length || overflow)return 1;
    kinfo("m5 shell selftest: command/mode/bounds/backspace/missing-ELF verified");return 0;
}
