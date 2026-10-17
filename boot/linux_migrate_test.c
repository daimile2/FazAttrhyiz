#include "capmix/kernel.h"
#include "capmix/elf.h"
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
int cm_linux_migrate_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("linux migration skipped: persona disabled");return 0;}
    cm_file_t *f=NULL;int e=cm_vfs_open("/bin/linux-migrate.elf",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);
    if(e)return 1;
    int64_t n=cm_vfs_write(f,cm_linux_migrate_elf,sizeof(cm_linux_migrate_elf));
    e=cm_vfs_fsync(f);cm_vfs_close(f);
    if(e || n!=(int64_t)sizeof(cm_linux_migrate_elf))return 1;
    unsigned errors=0;
    for(unsigned rollback=0;rollback<2;rollback++) {
        uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),charge,limit,after;
        cm_commit_stats(&charge,&limit);
        cm_spawn_attr_t a={.path="/bin/linux-migrate.elf",.qos=CM_QOS_USER_INTERACTIVE};
        cm_id_t pid=0;if(cm_posix_spawn(&a,&pid))return 1;
        cm_proc_t *p=cm_proc_lookup(pid);if(!p)return 1;
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
            probe=p;complete=0;p->linux_migrate_fail_once=(int)rollback;
            cm_smp_sched_disable();
            if(cm_smp_buddy_test(worker)!=cm_cpu_count())cm_panic("migration rendezvous failed");
            uint64_t *report=cm_elf_pointer(p->elf,0x401000);
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
        if(after!=charge || cm_mm_free_pages()+cm_file_cache_pages()!=pages)errors++;
    }
#ifdef CAPMIX_HOSTED
    kinfo("linux migration hosted: validation/group-denied verified; hardware handoff skipped");
#endif
    kinfo("linux migration selftest: %s (%u failures) pages/commit/reap verified",errors?"FAIL":"PASS",errors);
    return (int)errors;
}
