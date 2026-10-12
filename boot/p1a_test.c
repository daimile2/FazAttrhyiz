#include "capmix/kernel.h"
#include "capmix/fs.h"
#include "capmix/uaccess.h"
#include <string.h>
#include "../build/file_elf.inc"
#define CHECK(x) do {if(!(x)){kerr("p1a line %d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
int cm_p1a_selftest(void)
{
    int failed=0;cm_file_t *f=NULL;cm_elf_image_t *im=NULL;cm_id_t pid=0;
    CHECK(cm_vfs_open("/bin/filetest.elf",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f)==CM_OK);
    CHECK(cm_vfs_write(f,cm_file_elf,sizeof(cm_file_elf))==(int64_t)sizeof(cm_file_elf));
    CHECK(cm_vfs_fsync(f)==CM_OK);cm_vfs_close(f);f=NULL;
    CHECK(cm_elf_load("/bin/filetest.elf",&im)==CM_OK);
    char buf[16];
    CHECK(cm_copyin(im,buf,0x401000,5)==CM_OK && !memcmp(buf,"/p1a",5));
    CHECK(cm_copyout(im,0x400000,buf,1)==CM_ERR_INVAL);
    CHECK(cm_copyin(im,buf,UINT64_MAX-7,16)==CM_ERR_INVAL);
    CHECK(cm_copyin(im,buf,0xffffffff80000000ull,1)==CM_ERR_INVAL);
    CHECK(!cm_user_access(im,0x401000,CM_USER_IO_MAX+1,0));
    char *edge=cm_elf_pointer(im,0x405ffe);CHECK(edge);
    edge[0]='/';edge[1]=0;
    CHECK(cm_copyinstr(im,buf,0x405ffe,sizeof(buf))==CM_OK && buf[0]=='/' && !buf[1]);
    edge[1]='/';
    CHECK(cm_copyinstr(im,buf,0x405ffe,sizeof(buf))==CM_ERR_INVAL);
    CHECK(!cm_user_access(im,0x405fff,2,0));
    cm_elf_destroy(im);im=NULL;
#ifndef CAPMIX_HOSTED
    cm_spawn_attr_t attr={.path="/bin/filetest.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==CM_OK);
    cm_proc_t *p=cm_proc_lookup(pid);
    for(unsigned step=0;step<512 && (!p->exited || (p->main && p->main->id));step++)
        (void)cm_sched_run_until_idle(1);
    CHECK(p->exited && p->elf_started && p->exit_code==0);
    CHECK(cm_proc_reap_elf(pid)==CM_OK);pid=0;
    CHECK(cm_vfs_open("/p1a/user.txt",CM_O_RDONLY,&f)==CM_OK);
    char marker[64]={0};CHECK(cm_vfs_read(f,marker,sizeof(marker))==32);
    CHECK(!memcmp(marker,"CapMix P1a ELF file persistence\n",31));
    kinfo("p1a ELF file readback verified: CPL=3 open/read/write/close/mkdir/unlink/fsync");
#else
    kinfo("p1a hosted copyin/out invalid-pointer verified; ring3 file execution skipped");
#endif
cleanup:
    if(f)cm_vfs_close(f);
    cm_elf_destroy(im);
    if(pid) {
        cm_proc_t *unfinished=cm_proc_lookup(pid);
        if(unfinished && unfinished->main && unfinished->main->id){unfinished->exited=1;unfinished->main->state=CM_TH_DEAD;cm_thread_reap(unfinished->main);}
        (void)cm_proc_reap_elf(pid);
    }
    kinfo("p1a selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
