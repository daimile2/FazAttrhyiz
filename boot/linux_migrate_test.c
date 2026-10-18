#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/boot.h"
#include "capmix/linux_socket.h"
#include "capmix/linux_pipe.h"
#include "capmix/tcp.h"
#include "capmix/fs.h"
#include "capmix/uaccess.h"
#include <string.h>
#include "../build/linux_migrate.inc"
#ifndef CAPMIX_HOSTED
static cm_proc_t *probe;
static unsigned complete;
static void worker(unsigned cpu)
{
    uint64_t deadline=cm_rdtsc()+cm_tsc_hz()*8;
    while(!__atomic_load_n(&complete,__ATOMIC_ACQUIRE) && cm_rdtsc()<deadline) {
        (void)cm_sched_run_until_idle(1);
        if(cpu==0 && __atomic_load_n(&probe->exited,__ATOMIC_ACQUIRE) &&
           (!probe->main || __atomic_load_n(&probe->main->id,__ATOMIC_ACQUIRE)!=probe->elf_thread_id))
            __atomic_store_n(&complete,1,__ATOMIC_RELEASE);
    }
    cm_sched_release_current();
    if(!__atomic_load_n(&complete,__ATOMIC_ACQUIRE)) {
        kerr("migration timeout cpu=%u state=%d stage=%d placement=%d last=%llu depth=%d migrations=%u",cpu,probe->main->state,probe->linux_migrate,probe->elf_cpu,(unsigned long)probe->linux_last_syscall,probe->syscall_depth,probe->linux_migrations);
        cm_panic("linux migration timeout: refusing live-context reclaim");
    }
}
#endif
static int endpoint_guards(cm_proc_t *p)
{
    unsigned failed=0;cm_object_t *held=NULL;cm_task_t *foreign=NULL;int foreign_hid=-1;
    int64_t fd=-1;int pipefd[2]={-1,-1};uint64_t mask=1,u=0x7fc000;
#define GUARD(x) do{if(!(x)){kerr("migration endpoint guard line=%d",__LINE__);failed++;goto cleanup;}}while(0)
    uint64_t irq=cm_irq_save();cm_sched_set_current(p->main);
    GUARD(!cm_copyout(p->elf,u,&mask,8));
    fd=cm_linux_dispatch(41,2,2,0,0);GUARD(fd>=3);
    GUARD(cm_linux_dispatch(22,u+16,0,0,0)==0);
    GUARD(!cm_copyin(p->elf,pipefd,u+16,8));
    GUARD(cm_linux_dispatch(33,(uint64_t)fd,20,0,0)==20);
    GUARD(cm_linux_dispatch(203,0,8,u,0)==0); /* same CPU, including dup */
    for(unsigned i=0;i<2;i++) {
        int handle=p->fd[i?pipefd[0]:fd];
        GUARD(!cm_handle_acquire(p->task,handle,i?CM_OBJ_LINUX_PIPE:CM_OBJ_LINUX_SOCKET,0,&held));
        GUARD(cm_linux_dispatch(203,0,8,u,0)==-11); /* in-flight reference */
        cm_obj_release(held);held=NULL;
        foreign=cm_page_alloc(1);GUARD(foreign);memset(foreign,0,2*CM_PAGE_SIZE);
        GUARD(!cm_handle_duplicate(p->task,handle,foreign,&foreign_hid,CM_RIGHT_ALL));
        GUARD(cm_linux_dispatch(203,0,8,u,0)==-11); /* external same-end alias */
        GUARD(!cm_handle_drop(foreign,foreign_hid));foreign_hid=-1;cm_page_free(foreign,1);foreign=NULL;
        GUARD(cm_linux_dispatch(203,0,8,u,0)==0);
    }
    p->syscall_depth=2;int64_t nested=cm_linux_dispatch(203,0,8,u,0);p->syscall_depth=0;GUARD(nested==-11);
    p->linux_grouped=1;int64_t grouped=cm_linux_dispatch(203,0,8,u,0);p->linux_grouped=0;GUARD(grouped==-95);
    kinfo("linux migration endpoint guards: dup=allowed in-flight/foreign-alias/nested=EAGAIN group=EOPNOTSUPP verified");
cleanup:
    p->syscall_depth=0;p->linux_grouped=0;
    if(held)cm_obj_release(held);
    if(foreign){if(foreign_hid>0)(void)cm_handle_drop(foreign,foreign_hid);cm_page_free(foreign,1);}
    if(fd>=0)(void)cm_linux_dispatch(3,(uint64_t)fd,0,0,0);
    (void)cm_linux_dispatch(3,20,0,0,0);
    for(unsigned i=0;i<2;i++)if(pipefd[i]>=0)(void)cm_linux_dispatch(3,(uint64_t)pipefd[i],0,0,0);
    cm_sched_set_current(NULL);cm_irq_restore(irq);
#undef GUARD
    return (int)failed;
}
int cm_linux_migrate_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("linux migration skipped: persona disabled");return 0;}
    cm_file_t *f=NULL;int e=cm_vfs_open("/bin/linux-migrate.elf",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);
    if(e)return 1;
    int64_t n=cm_vfs_write(f,cm_linux_migrate_elf,sizeof(cm_linux_migrate_elf));
    e=cm_vfs_fsync(f);cm_vfs_close(f);
    if(e || n!=(int64_t)sizeof(cm_linux_migrate_elf))return 1;
    unsigned errors=0;
    cm_iface_t *nic=NULL;
    for(unsigned i=0;i<2;i++){cm_iface_t *x=cm_if_get(cm_if_find(i?"enet0":"vnet0"));if(x && x->up){nic=x;break;}}
    int wire=nic && cm_boot_has_option("capmix.tcp_echo=1");
    if(!wire)kinfo("linux migration TCP wire skipped: no up NIC or capmix.tcp_echo=1 not set; pipe/UDP still tested");
    for(unsigned rollback=0;rollback<2;rollback++) {
        unsigned sockets=cm_linux_socket_live(),pipes=cm_linux_pipe_live(),tcp=cm_tcp_live();
        uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),charge,limit,after;
        cm_commit_stats(&charge,&limit);
        const char *args[]={"/bin/linux-migrate.elf","wire",NULL};
        cm_spawn_attr_t a={.path=args[0],.argv=wire?args:NULL,.qos=CM_QOS_USER_INTERACTIVE};
        cm_id_t pid=0;if(cm_posix_spawn(&a,&pid))return 1;
        cm_proc_t *p=cm_proc_lookup(pid);if(!p)return 1;
        errors+=(unsigned)endpoint_guards(p);
#ifdef CAPMIX_HOSTED
        cm_sched_set_current(p->main);
        uint64_t mask=1;cm_copyout(p->elf,0x401010,&mask,8);
        if(cm_linux_dispatch(203,pid+1,8,0x401010,0)!=-3 ||
           cm_linux_dispatch(203,0,0,0x401010,0)!=-22 ||
           cm_linux_dispatch(203,0,8,UINT64_MAX,0)!=-14 ||
           cm_linux_dispatch(203,0,8,0x401010,0)!=0)errors++;
        mask=3;cm_copyout(p->elf,0x401010,&mask,8);
        if(cm_linux_dispatch(203,0,8,0x401010,0)!=-22)errors++;
        mask=1ull<<63;cm_copyout(p->elf,0x401010,&mask,8);
        if(cm_linux_dispatch(203,0,8,0x401010,0)!=-22)errors++;
        mask=1;cm_copyout(p->elf,0x401010,&mask,8);p->linux_fd_owner=pid;
        if(cm_linux_dispatch(203,0,8,0x401010,0)!=-11)errors++;
        p->linux_fd_owner=0;p->linux_fork_child=1;
        if(cm_linux_dispatch(203,0,8,0x401010,0)!=-95)errors++;
        p->linux_fork_child=0;p->linux_grouped=1;
        if(cm_linux_dispatch(203,0,8,0x401010,0)!=-95)errors++;
        p->linux_grouped=0;cm_proc_finish_exit(p,0);cm_sched_set_current(NULL);
        cm_thread_reap(p->main);
#else
        if(cm_cpu_count()<2) {
            cm_proc_finish_exit(p,0);cm_thread_reap(p->main);
            kinfo("linux migration skipped: single CPU (request rejection only)");
        } else {
            cm_elf_image_t *snapshot=NULL;
            if(cm_elf_clone_cow(p->elf,&snapshot))cm_panic("migration test COW snapshot allocation");
            uint64_t splits=cm_elf_cow_splits();
            if(wire && cm_net_bg_start())cm_panic("migration RX worker start failed");
            probe=p;complete=0;p->linux_migrate_fail_once=(int)rollback;
            cm_smp_sched_disable();
            if(cm_smp_buddy_test(worker)!=cm_cpu_count())cm_panic("migration rendezvous failed");
            if(wire)cm_net_bg_stop();
            uint64_t *report=cm_elf_pointer(p->elf,0x401000);
            kinfo("linux migration progress: exit=%d report=%llu calls=%llu",p->exit_code,(unsigned long)report[0],(unsigned long)p->linux_calls);
            if(p->exit_code || report[0]!=1 || report[1]!=rollback || p->elf_cpu!=0 ||
               p->linux_migrations!=8 || p->linux_migrate_rollbacks!=rollback || p->linux_migrate ||
               *(uint64_t *)cm_elf_pointer(snapshot,0x402000)!=0 ||
               *(uint64_t *)cm_elf_pointer(p->elf,0x402000)!=0x5566 || cm_elf_cow_splits()<=splits)errors++;
            cm_elf_destroy(snapshot);
            kinfo("linux migration verified: CPU0->CPU1->CPU0 rounds=4 migrations=%u rollback=%s FS/TLS/COW/syscall/exit=%s",
                  p->linux_migrations,rollback?"ok":"not-injected",errors?"FAIL":"ok");
        }
#endif
        if(cm_proc_release_elf(pid))errors++;
        cm_commit_stats(&after,&limit);
        if(sockets!=cm_linux_socket_live() || pipes!=cm_linux_pipe_live() || tcp!=cm_tcp_live())errors++;
        if(after!=charge || cm_mm_free_pages()+cm_file_cache_pages()!=pages)errors++;
    }
#ifdef CAPMIX_HOSTED
    kinfo("linux migration hosted: validation/group-denied verified; hardware handoff skipped");
#endif
    kinfo("linux migration selftest: %s (%u failures) pages/commit/reap verified",errors?"FAIL":"PASS",errors);
    return (int)errors;
}
