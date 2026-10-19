#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/linux_uapi.h"
#include "capmix/uaccess.h"
#include "capmix/numa.h"
#include <string.h>
#define CHECK(x) do{if(!(x)){kerr("linux compat line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
int cm_linux_compat_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("linux compat skipped: persona disabled");return 0;}
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;
    cm_spawn_attr_t a={.path="/bin/linux-hello.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&a,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    cm_sched_set_current(p->main);
    uint64_t u=0x7fc000,all=UINT64_MAX,v=0,blocked=all&~((1ull<<8)|(1ull<<18));
    CHECK(cm_copyout(p->elf,u,&all,8)==0);
    CHECK(cm_linux_dispatch(14,2,u,u+64,8)==0 && p->linux_sigmask==blocked);
    CHECK(cm_copyin(p->elf,&v,u+64,8)==0 && v==0);
    CHECK(cm_linux_dispatch(14,99,0,u+64,8)==0);
    CHECK(cm_copyin(p->elf,&v,u+64,8)==0 && v==blocked);
    CHECK(cm_linux_dispatch(14,1,u,0,8)==0 && !p->linux_sigmask);
    CHECK(cm_linux_dispatch(14,0,u,0,8)==0 && p->linux_sigmask==blocked);
    CHECK(cm_linux_dispatch(14,3,u,0,8)==-22);
    CHECK(cm_linux_dispatch(14,0,u,0,128)==-22);
    CHECK(cm_linux_dispatch(14,2,UINT64_MAX,0,8)==-14);
    CHECK(cm_linux_dispatch(14,1,u,UINT64_MAX,8)==-14 && p->linux_sigmask==blocked);
    cm_linux_sigaction_t act={.handler=1,.flags=0x04000000};
    CHECK(cm_copyout(p->elf,u,&act,sizeof(act))==0);
    CHECK(cm_linux_dispatch(13,2,u,u+64,8)==0);
    CHECK(cm_linux_dispatch(13,2,0,u+64,8)==0);
    CHECK(cm_copyin(p->elf,&act,u+64,sizeof(act))==0 && act.handler==1);
    act.handler=u;CHECK(cm_copyout(p->elf,u,&act,sizeof(act))==0);
    CHECK(cm_linux_dispatch(13,2,u,0,8)==-95);
    CHECK(cm_linux_dispatch(13,9,0,u,8)==-22);
    CHECK(cm_linux_dispatch(13,65,0,u,8)==-22);
    CHECK(cm_linux_dispatch(13,2,UINT64_MAX,0,8)==-14);
    CHECK(cm_linux_dispatch(13,2,0,UINT64_MAX,8)==-14);
    CHECK(p->linux_sigignored==(1ull<<1) && !p->linux_enosys);
    CHECK(cm_linux_dispatch(309,u,u+4,0,0)==0);
    uint32_t location[2];CHECK(cm_copyin(p->elf,location,u,8)==0);
    CHECK(location[0]==(unsigned)cm_cpu_id() && location[1]==(unsigned)cm_numa_cpu_node(cm_cpu_id()));
    CHECK(cm_linux_dispatch(309,UINT64_MAX,0,0,0)==-14);
    CHECK(cm_linux_dispatch(204,0,8,u,0)==8);
    CHECK(cm_copyin(p->elf,&v,u,8)==0 && v==(cm_cpu_count()>=64?UINT64_MAX:(1ull<<cm_cpu_count())-1));
    CHECK(cm_linux_dispatch(204,0,7,u,0)==-22);
    CHECK(cm_linux_dispatch(204,0,8,UINT64_MAX,0)==-14);
    CHECK(cm_linux_dispatch(204,UINT64_MAX,8,u,0)==-3);
    kinfo("linux CPU query nr=204/309: cpus=%d eligible-mask/getcpu/NUMA/copy/errno verified",cm_cpu_count());
    static uint64_t before[CM_LINUX_ENOSYS_BUCKETS],after[CM_LINUX_ENOSYS_BUCKETS];
    cm_linux_enosys_snapshot(before);
    CHECK(cm_linux_dispatch(41,2,3,0,0)==-38); /* unsupported socket type helper */
    CHECK(cm_linux_dispatch(9999,0,0,0,0)==-38);
    cm_linux_enosys_snapshot(after);
    CHECK(after[41]==before[41]+1 && after[512]==before[512]+1 && p->linux_enosys==2);
    CHECK(after[13]==before[13] && after[14]==before[14]);
    for(unsigned i=0;i<CM_LINUX_ENOSYS_LOG_LIMIT+3;i++)CHECK(cm_linux_dispatch(511,0,0,0,0)==-38);
    cm_linux_enosys_snapshot(after);
    CHECK(after[511]==before[511]+CM_LINUX_ENOSYS_LOG_LIMIT+3);
    CHECK(p->linux_enosys==CM_LINUX_ENOSYS_LOG_LIMIT+5);
    kinfo("linux ENOSYS budget: text=%u histogram nr=511 count=%u beyond-budget verified",CM_LINUX_ENOSYS_LOG_LIMIT,CM_LINUX_ENOSYS_LOG_LIMIT+3);
    cm_linux_enosys_dump("negative",before);
    kinfo("linux compat accounting: helper nr=41 + unknown overflow counted once verified");
    kinfo("linux compat nr=13/14: mask/action/copy/errno verified; handlers=EOPNOTSUPP delivery=SIGCHLD-only");
cleanup:
    cm_sched_set_current(NULL);
    if(p){cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    kinfo("linux compat selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
