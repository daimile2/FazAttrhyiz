#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/argv_elf.inc"
#define CHECK(x) do{if(!(x)){kerr("argv line %d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
static int string_equal(cm_elf_image_t *im,uint64_t address,const char *expected)
{
    char buffer[256];size_t n=strlen(expected)+1;return n<=sizeof(buffer) &&
        !cm_copyin(im,buffer,address,n) && !memcmp(buffer,expected,n);
}
int cm_argv_selftest(void)
{
    int failed=0;cm_file_t *file=NULL;cm_elf_image_t *im=NULL;cm_proc_t *p=NULL;cm_id_t pid=0;
    uint64_t before=cm_mm_free_pages();
    CHECK(cm_vfs_open("/bin/argtest.elf",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&file)==CM_OK);
    CHECK(cm_vfs_write(file,cm_argv_elf,sizeof(cm_argv_elf))==(int64_t)sizeof(cm_argv_elf));
    CHECK(cm_vfs_fsync(file)==CM_OK);cm_vfs_close(file);file=NULL;before=cm_mm_free_pages();
    CHECK(cm_elf_load("/bin/argtest.elf",&im)==CM_OK);
    uint64_t words[CM_ELF_ARG_MAX+CM_ELF_ENV_MAX+5];
    CHECK(im->argc==1 && im->envc==0 && !(im->stack&15));
    CHECK(cm_copyin(im,words,im->stack,48)==CM_OK && words[0]==1 && !words[2] && !words[3] && !words[4] && !words[5]);
    CHECK(string_equal(im,words[1],"/bin/argtest.elf"));
    /* Max-length strings and a vector crossing two separately backed stack pages. */
    static char large[CM_ELF_ARG_STR_MAX];memset(large,'x',sizeof(large)-1);large[sizeof(large)-1]=0;
    const char *args[CM_ELF_ARG_MAX+2];for(unsigned i=0;i<CM_ELF_ARG_MAX;i++)args[i]=large;args[CM_ELF_ARG_MAX]=NULL;
    uint64_t old_stack=im->stack;
    CHECK(cm_elf_set_args(im,args,NULL)==CM_ERR_NOSPC && im->stack==old_stack);
    for(unsigned i=0;i<CM_ELF_ARG_MAX+1;i++)args[i]="x";
    args[CM_ELF_ARG_MAX+1]=NULL;
    CHECK(cm_elf_set_args(im,args,NULL)==CM_ERR_NOSPC && im->stack==old_stack);
    static char long_string[CM_ELF_ARG_STR_MAX+1];memset(long_string,'z',sizeof(long_string));
    const char *bad[]={long_string,NULL};CHECK(cm_elf_set_args(im,bad,NULL)==CM_ERR_NOSPC);
    static char tail[240];memset(tail,'y',sizeof(tail)-1);tail[sizeof(tail)-1]=0;
    for(unsigned i=0;i<15;i++)args[i]=large;
    args[15]=tail;args[16]=NULL;
    CHECK(cm_elf_set_args(im,args,NULL)==CM_OK && im->argc==16);
    CHECK(cm_copyin(im,words,im->stack,21*8)==CM_OK && words[0]==16);
    for(unsigned i=0;i<15;i++)CHECK(string_equal(im,words[1+i],large));
    CHECK(string_equal(im,words[16],tail) && !words[17] && !words[18] && !words[19] && !words[20]);
    const char *empty[]={NULL};CHECK(cm_elf_set_args(im,empty,NULL)==CM_OK && im->argc==1);
    cm_elf_destroy(im);im=NULL;CHECK(cm_mm_free_pages()==before);
    cm_spawn_attr_t rejected={.path="/bin/argtest.elf",.argv=bad,.qos=CM_QOS_DEFAULT};
    cm_id_t unchanged=99;
    CHECK(cm_posix_spawn(&rejected,&unchanged)==CM_ERR_NOSPC && unchanged==99 && cm_mm_free_pages()==before);
    /* Failed preparation must not consume a published process slot. */
    kinfo("argv stack verified: argc>=1 default path, 16-byte alignment, cross-page vector, count/string/aggregate limits rollback");
    char copied[]="hello";const char *argv[]={"/bin/argtest.elf",copied,"","world",NULL};
    const char *envp[]={"CAPMIX_TEST=argv","MODE=debug",NULL};
    cm_spawn_attr_t attr={.path="/bin/argtest.elf",.argv=argv,.envp=envp,.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==CM_OK);p=cm_proc_lookup(pid);CHECK(p && p->elf);
    copied[0]='X';CHECK(p->elf->argc==4 && p->elf->envc==2);
    CHECK(cm_copyin(p->elf,words,p->elf->stack,11*8)==CM_OK && words[0]==4 && !words[5] && !words[8] && !words[9] && !words[10]);
    CHECK(string_equal(p->elf,words[1],"/bin/argtest.elf") && string_equal(p->elf,words[2],"hello"));
    CHECK(string_equal(p->elf,words[3],"") && string_equal(p->elf,words[4],"world"));
    CHECK(string_equal(p->elf,words[6],"CAPMIX_TEST=argv") && string_equal(p->elf,words[7],"MODE=debug"));
    for(unsigned step=0;step<512 && (!p->exited || (p->main && p->main->id));step++)(void)cm_sched_run_until_idle(1);
    CHECK(p->exited);
#ifndef CAPMIX_HOSTED
    CHECK(p->elf_started && p->exit_code==0);
    uint64_t report[4];CHECK(cm_copyin(p->elf,report,0x401000,sizeof(report))==CM_OK);
    CHECK(report[0]==4 && report[1]==3 && report[2]==2 && report[3]==1);
    kinfo("argv ELF verified: CPL=3 argc=4 argv[0]/hello/empty/world envc=2 crt main exit=0");
#else
    CHECK(!p->elf_started && p->exit_code==CM_ERR_NOTSUP);
    kinfo("argv hosted: stack/argv/envp snapshot verified; CPL=3 crt execution skipped");
#endif
    CHECK(cm_proc_reap_elf(pid)==CM_OK);pid=0;
cleanup:
    if(file)cm_vfs_close(file);
    cm_elf_destroy(im);
    if(pid && p){if(p->main && p->main->id){p->exited=1;p->main->state=CM_TH_DEAD;cm_thread_reap(p->main);}(void)cm_proc_reap_elf(pid);}
    if(cm_mm_free_pages()!=before){kerr("argv image/argument snapshot leak");failed++;}
    kinfo("argv selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
