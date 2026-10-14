#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/io.h"
#include <string.h>
#include "../build/concurrent_elf.inc"
#include "../build/blockd_wait_elf.inc"
extern void cm_commit_stats(uint64_t *,uint64_t *);
#define CHECK(x) do{if(!(x)){kerr("elf concurrency line %d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
static int put(const char *path,const void *data,size_t bytes)
{
    cm_file_t *f=NULL;int e=cm_vfs_open(path,CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f);if(e)return e;
    int64_t n=cm_vfs_write(f,data,bytes);e=n==(int64_t)bytes?cm_vfs_fsync(f):CM_ERR_IO;cm_vfs_close(f);return e;
}
static int done(cm_proc_t *p)
{
    return p && __atomic_load_n(&p->exited,__ATOMIC_ACQUIRE) &&
        (!p->main || __atomic_load_n(&p->main->id,__ATOMIC_ACQUIRE)!=p->elf_thread_id);
}
static int wait_pair(cm_proc_t *a,cm_proc_t *b)
{
    uint64_t hz=cm_tsc_hz();if(!hz)hz=1000000000ull;
    uint64_t deadline=cm_rdtsc()+hz*5;
    while(!done(a) || !done(b)) {
        if(cm_rdtsc()>deadline)return 0;
        (void)cm_sched_run_until_idle(1);
    }
    return 1;
}
static int overlap_test(void)
{
    int failed=0;cm_id_t ids[2]={0};cm_proc_t *p[2]={0};
    uint64_t before=cm_mm_free_pages();
    for(unsigned i=0;i<2;i++) {
        cm_spawn_attr_t attr={.path="/bin/concurrent.elf",.qos=CM_QOS_USER_INTERACTIVE,.cpu_set=1,
                             .cpu=(int)(i && cm_cpu_count()>1)};
        CHECK(cm_posix_spawn(&attr,&ids[i])==CM_OK);p[i]=cm_proc_lookup(ids[i]);CHECK(p[i] && p[i]->elf);
        uint64_t *config=cm_elf_pointer(p[i]->elf,0x401000);CHECK(config);
        config[0]=cm_tsc_hz()/5;config[1]=0x13579+i;
    }
    CHECK(p[0]->elf->cr3!=p[1]->elf->cr3 && p[0]->elf_entry_slot!=p[1]->elf_entry_slot);
#ifndef CAPMIX_HOSTED
    cm_smp_sched_enable();
#endif
    CHECK(wait_pair(p[0],p[1]));
#ifndef CAPMIX_HOSTED
    uint64_t *a=cm_elf_pointer(p[0]->elf,0x401000),*b=cm_elf_pointer(p[1]->elf,0x401000);
    CHECK(p[0]->exit_code==0 && p[1]->exit_code==0 && a[5]==3 && b[5]==3 && a[4] && b[4]);
    CHECK(a[1]==0x13579 && b[1]==0x1357a);
    uint64_t start=CM_MAX(a[2],b[2]),end=CM_MIN(a[3],b[3]);CHECK(end>start);
    CHECK(p[0]->elf_preempts && p[1]->elf_preempts);
    kinfo("elf concurrent overlap verified: cpu=%d/%d overlap_cycles=%llu CPL=3 private memory/registers/stack exit=0/0",
        p[0]->elf_cpu,p[1]->elf_cpu,(unsigned long)(end-start));
#else
    CHECK(p[0]->exit_code==CM_ERR_NOTSUP && p[1]->exit_code==CM_ERR_NOTSUP);
    kinfo("elf concurrent hosted: dual image/publication/affinity/reclaim verified; execution overlap skipped (no ring3)");
#endif
cleanup:
    cm_smp_sched_disable();
    /* Successful runs are already off CPU. Never force-reap a live AP context. */
    for(unsigned i=0;i<2;i++)if(ids[i] && done(p[i]) && cm_proc_release_elf(ids[i])!=CM_OK)failed++;
    if(cm_mm_free_pages()!=before){kerr("elf concurrent reclaim page mismatch before=%llu after=%llu",(unsigned long)before,(unsigned long)cm_mm_free_pages());cm_sched_dump();failed++;}
    return failed;
}
static int blocked_test(void)
{
    int failed=0,did=-1,was_online=cm_blk_online();cm_id_t a=0,b=0;cm_proc_t *p=NULL,*q=NULL;
    uint64_t before=cm_mm_free_pages();
    cm_spawn_attr_t attr={.path="/bin/concurrent-blockd.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&a)==CM_OK);p=cm_proc_lookup(a);CHECK(p && p->elf);
    int send,recv;CHECK(cm_channel_create(p->task,&send,&recv)==CM_OK);
    did=cm_drv_bind_channel("concurrent-blockd",12,a,send,recv);CHECK(did>=0);
    CHECK(cm_handle_drop(p->task,send)==CM_OK);
    uint32_t *config=cm_elf_pointer(p->elf,0x401000);config[0]=(uint32_t)recv;config[1]=(uint32_t)did;
#ifndef CAPMIX_HOSTED
    for(unsigned n=0;n<64 && p->main->state!=CM_TH_BLOCKED;n++)(void)cm_sched_run_until_idle(1);
    CHECK(p->main->state==CM_TH_BLOCKED);
#endif
    attr.path="/bin/concurrent.elf";
    CHECK(cm_posix_spawn(&attr,&b)==CM_OK);q=cm_proc_lookup(b);CHECK(q && q->elf);
    uint64_t *other=cm_elf_pointer(q->elf,0x401000);other[0]=cm_tsc_hz()/20;
    for(unsigned n=0;n<512 && !done(q);n++)(void)cm_sched_run_until_idle(1);
    CHECK(done(q));
#ifndef CAPMIX_HOSTED
    CHECK(q->exit_code==0 && p->main->state==CM_TH_BLOCKED && !p->exited);
    kinfo("elf same-cpu isolation: A sleeping syscall=24; B executed/exited cpu=0 while A blocked");
#endif
    cm_irq_inject(12);CHECK(cm_irq_softirq_flush()==1);
    CHECK(wait_pair(p,q));
#ifndef CAPMIX_HOSTED
    uint64_t *report=cm_elf_pointer(p->elf,0x401010);
    CHECK(p->exit_code==0 && report[0]==1 && report[1]==3 && report[2]==0);
    CHECK(cm_drv_get(did)->wakes==1 && cm_drv_get(did)->received==1);
    CHECK(p->elf_start_tsc<q->elf_start_tsc && q->elf_end_tsc<p->elf_end_tsc);
    kinfo("elf same-cpu blocked syscall isolation verified: sleep -> B exit -> IRQ wake -> recv; user_rsp/registers/stack/CR3 exit=0 overlap=1");
#else
    cm_drv_process_exit(a);kinfo("elf same-cpu hosted: blocking execution skipped (no ring3)");
#endif
cleanup:
    if(was_online){extern void cm_blk_set_offline(int);cm_blk_set_offline(0);}
    for(unsigned i=0;i<2;i++){cm_id_t id=i?b:a;cm_proc_t *proc=i?q:p;if(id && done(proc)) {if(cm_proc_release_elf(id)!=CM_OK)failed++;}}
    if(cm_mm_free_pages()!=before){kerr("elf blocked reclaim page leak");failed++;}
    return failed;
}
static int creation_rollback_test(void)
{
    cm_id_t ids[CM_MAX_PROCS]={0};unsigned n=0;int failed=0;
    uint64_t before=cm_mm_free_pages(),charge,limit,after;
    cm_commit_stats(&charge,&limit);
    cm_spawn_attr_t attr={.path="/bin/concurrent.elf",.qos=CM_QOS_USER_INTERACTIVE,
                         .cpu_set=1,.cpu=cm_cpu_count()};
    cm_id_t unchanged=99;
    if(cm_posix_spawn(&attr,&unchanged)!=CM_ERR_INVAL || unchanged!=99 || cm_mm_free_pages()!=before)failed++;
    attr.cpu=0;
    for(;n<CM_MAX_PROCS;n++) {
        cm_error_t e=cm_posix_spawn(&attr,&ids[n]);
        if(e) {if(e!=CM_ERR_NOSPC || ids[n])failed++;break;}
    }
    if(n==CM_MAX_PROCS)failed++;
    /* No scheduler ran: thread stacks may safely be retired before any IRET. */
    for(unsigned i=0;i<n;i++) {
        cm_proc_t *p=cm_proc_lookup(ids[i]);
        p->exit_code=CM_ERR_AGAIN;p->exited=1;p->main->state=CM_TH_DEAD;
        cm_thread_reap(p->main);
        if(cm_proc_release_elf(ids[i])!=CM_OK)failed++;
    }
    cm_commit_stats(&after,&limit);
    if(cm_mm_free_pages()!=before || after!=charge)failed++;
    kinfo("elf create/failure/reap verified: slots=%u capacity rejection=NOSPC image/page-tables/native-stack/entry-slot leak=%d",
          n,failed);
    return failed;
}
int cm_concurrent_selftest(void)
{
    int e=put("/bin/concurrent.elf",cm_concurrent_elf,sizeof(cm_concurrent_elf));
    if(!e)e=put("/bin/concurrent-blockd.elf",cm_blockd_wait_elf,sizeof(cm_blockd_wait_elf));
    if(e)return 1;
    /* Earlier IPC tests leave backgroundd runnable. Complete it before the
     * exact free-page baseline; APs would otherwise reclaim its 4-page
     * native stack during the overlap test (a gain, not an ELF leak). */
    (void)cm_sched_run_until_idle(512);
    if(cm_sched_runnable()){kerr("elf concurrency baseline not quiescent");return 1;}
    int failed=creation_rollback_test();if(!failed)failed+=overlap_test();if(!failed)failed+=blocked_test();
    kinfo("elf concurrency selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
