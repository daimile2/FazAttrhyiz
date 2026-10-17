#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/sha256.h"
#include <string.h>
#include "../build/micropython.inc"
#include "../build/micropython_demo.inc"
#define CHECK(x) do{if(!(x)){kerr("micropython line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
static int stage(const char *path,const void *data,size_t length)
{
    cm_file_t *f=NULL;int e=cm_vfs_open(path,CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);if(e)return e;
    if(cm_vfs_write(f,data,length)!=(int64_t)length)e=CM_ERR_IO;
    if(!e)e=cm_vfs_fsync(f);
    cm_vfs_close(f);return e;
}
#ifndef CAPMIX_HOSTED
static void close_output(void *f){cm_vfs_close(f);}
static int contains(const char *s,size_t n,const char *part)
{
    size_t len=strlen(part);if(len>n)return 0;
    for(size_t i=0;i<=n-len;i++)if(!memcmp(s+i,part,len))return 1;
    return 0;
}
/* Success cases compare all bytes. Error cases require a diagnostic suffix;
 * traceback depth may vary with compiler stack layout. Never reap a live CPU. */
static int run(const char *name,const char *code,int from_file,const char *expected,int status,int exact)
{
    int failed=0;cm_file_t *output=NULL;cm_object_t *object=NULL;cm_proc_t *p=NULL;cm_id_t pid=0;
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),charge,limit,after;
    unsigned files=cm_vfs_open_files();cm_commit_stats(&charge,&limit);
    CHECK(cm_vfs_open("/micropython-output",CM_O_CREAT|CM_O_TRUNC|CM_O_RDWR,&output)==0);
    object=cm_obj_mint_owned(CM_OBJ_FILE,output,CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE,close_output);CHECK(object);output=NULL;
    const char *inline_args[]={"/bin/micropython.elf","-c",code,NULL};
    const char *file_args[]={"/bin/micropython.elf",code,NULL};
    cm_spawn_attr_t a={.path=inline_args[0],.argv=from_file?file_args:inline_args,.qos=CM_QOS_USER_INTERACTIVE,.stdio={NULL,object}};
    CHECK(cm_posix_spawn(&a,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    uint64_t deadline=cm_rdtsc()+cm_tsc_hz()*5;
    while((!p->exited || p->main->id==p->elf_thread_id) && cm_rdtsc()<deadline)(void)cm_sched_run_until_idle(1);
    if(!p->exited || p->main->id==p->elf_thread_id)cm_panic("micropython timeout: refusing live user-context reclaim");
    kinfo("micropython state: case=%s exit=%lld calls=%llu ENOSYS=%llu",name,(long long)p->exit_code,(unsigned long)p->linux_calls,(unsigned long)p->linux_enosys);
    CHECK(p->exit_code==status && p->linux_enosys==0);
    static char bytes[2048];memset(bytes,0,sizeof(bytes));
    int64_t n=cm_vfs_pread(object->payload,bytes,sizeof(bytes)-1,0);CHECK(n>=0);
    if(exact)CHECK(n==(int64_t)strlen(expected) && !memcmp(bytes,expected,(size_t)n));
    else CHECK(contains(bytes,(size_t)n,expected));
    CHECK(cm_vfs_fsync(object->payload)==0);
    kinfo("micropython %s: CPL=3 output verified exit=%d ENOSYS=0",name,status);
cleanup:
    if(output)cm_vfs_close(output);
    if(object)cm_obj_release(object);
    if(p) {
        if(!p->exited || (p->main && p->main->id==p->elf_thread_id))cm_panic("micropython cleanup: refusing live-context reclaim");
        if(cm_proc_release_elf(pid))failed++;
    }
    cm_commit_stats(&after,&limit);
    if(after!=charge || cm_mm_free_pages()+cm_file_cache_pages()!=pages || cm_vfs_open_files()!=files) {
        kerr("micropython reclaim mismatch: case=%s",name);failed++;
    } else kinfo("micropython reclaim: case=%s pages/commit/fds=ok",name);
    return failed;
}
#endif
int cm_linux_micropython_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("micropython skipped: persona disabled; native programs retained");return 0;}
    int failed=0;cm_elf_image_t *image=NULL;uint8_t digest[32];char hex[65];static const char digits[]="0123456789abcdef";
    CHECK(cm_sha256(cm_micropython_elf,sizeof(cm_micropython_elf),digest)==0);
    for(unsigned i=0;i<32;i++){hex[2*i]=digits[digest[i]>>4];hex[2*i+1]=digits[digest[i]&15];}hex[64]=0;
    kinfo("micropython fixture: version=1.24.1 static-scalar sha256=%s bytes=%u GC=256KiB stack-check=8KiB",hex,(unsigned)sizeof(cm_micropython_elf));
    CHECK(stage("/bin/micropython.elf",cm_micropython_elf,sizeof(cm_micropython_elf))==0);
    CHECK(stage("/micropython-demo.py",cm_micropython_demo,sizeof(cm_micropython_demo))==0);
    CHECK(stage("/micropython-output","",0)==0);
    static char oversized[8193];memset(oversized,'x',sizeof(oversized));
    CHECK(stage("/micropython-large.py",oversized,sizeof(oversized))==0);
    static const char nul_script[]="pass\0hidden";
    CHECK(stage("/micropython-nul.py",nul_script,sizeof(nul_script)-1)==0);
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),charge,limit,after;cm_commit_stats(&charge,&limit);
    CHECK(cm_elf_load("/bin/micropython.elf",&image)==0 && image->abi==CM_ABI_LINUX);
    CHECK(image->npages>=64 && image->npages<256 && image->entry>=0x400000 && image->entry<0x7fc000);
    cm_elf_destroy(image);image=NULL;cm_commit_stats(&after,&limit);
    CHECK(after==charge && cm_mm_free_pages()+cm_file_cache_pages()==pages);
#ifdef CAPMIX_HOSTED
    kinfo("micropython hosted: ELF/persona/BSS/commit/reclaim verified; guest execution skipped");
#else
    static uint64_t enosys[CM_LINUX_ENOSYS_BUCKETS];cm_linux_enosys_snapshot(enosys);
    failed+=run("inline","print('micropython inline'); print(6*7); assert 6*7==42",0,"micropython inline\n42\n",0,1);
    failed+=run("file","/micropython-demo.py",1,"micropython golden\n42\n",0,1);
    failed+=run("exit7","import sys; print('micropython exit7'); sys.exit(7)",0,"micropython exit7\n",7,1);
    failed+=run("gc-integer","s=0\nfor i in range(4000):\n a=[i]*32\n s+=i\nassert s==7998000\nprint(s)\nprint(2**80)",0,"7998000\n1208925819614629174706176\n",0,1);
    failed+=run("syntax","if :",0,"SyntaxError: invalid syntax\n",1,0);
    failed+=run("heap-limit","a=[0]*1000000",0,"MemoryError: memory allocation failed",1,0);
    failed+=run("stack-limit","def f():\n return f()\nf()",0,"maximum recursion depth exceeded\n",1,0);
    failed+=run("missing-file","/micropython-missing.py",1,"script open failed\n",66,1);
    failed+=run("float-disabled","print(1.5)",0,"decimal numbers not supported",1,0);
    failed+=run("import-disabled","import socket",0,"ImportError:",1,0);
    failed+=run("file-limit","/micropython-large.py",1,"script too large\n",65,1);
    failed+=run("NUL-file","/micropython-nul.py",1,"script contains NUL\n",65,1);
    cm_linux_enosys_dump("micropython",enosys);
#endif
cleanup:
    cm_elf_destroy(image);
    kinfo("micropython selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
