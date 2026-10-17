#include "capmix/kernel.h"
#include "capmix/elf.h"
#include <string.h>
#ifndef CAPMIX_HOSTED
/* AP idle rendezvous is reused without modifying its dispatcher. Each callback
 * owns its local scheduler continuation; no force-reap of a live AP context. */
static unsigned ready,errors;
static uint64_t starts[CM_MAX_CPUS],ends[CM_MAX_CPUS];
static unsigned mode;
static int done(cm_proc_t *p)
{return __atomic_load_n(&p->exited,__ATOMIC_ACQUIRE) && (!p->main || __atomic_load_n(&p->main->id,__ATOMIC_ACQUIRE)!=p->elf_thread_id);}
static void worker(unsigned cpu)
{
    if(mode && cpu!=1)return;
    cm_id_t pid=0;cm_proc_t *p=NULL;int e;unsigned bad=0;
    const char *args[]={"/bin/linux-probe.elf",cpu&1?"B":"A",NULL};
    cm_spawn_attr_t a={.path=mode?"/bin/linux-l2.elf":args[0],.argv=mode?NULL:args,
        .qos=CM_QOS_USER_INTERACTIVE,.cpu_set=1,.cpu=(int)cpu};
    uint64_t deadline=cm_rdtsc()+cm_tsc_hz()*8;
    do {e=cm_posix_spawn(&a,&pid);if(e==CM_ERR_AGAIN)__asm__ volatile("pause");}while(e==CM_ERR_AGAIN && cm_rdtsc()<deadline);
    if(!e)p=cm_proc_lookup(pid);
    if(e || !p)bad=1;
    if(!mode) {
        if(p)*(uint64_t *)cm_elf_pointer(p->elf,0x401008)=cm_tsc_hz()/3;
        __atomic_add_fetch(&ready,1,__ATOMIC_RELEASE);
        while(__atomic_load_n(&ready,__ATOMIC_ACQUIRE)!=(unsigned)cm_cpu_count() && cm_rdtsc()<deadline)__asm__ volatile("pause");
        if(__atomic_load_n(&ready,__ATOMIC_ACQUIRE)!=(unsigned)cm_cpu_count())bad=1;
    }
    if(p) {
        while(!done(p) && cm_rdtsc()<deadline)(void)cm_sched_run_until_idle(1);
        if(!done(p))cm_panic("linux SMP timeout: refusing to reclaim a live user context");
        if(p->exit_code || p->elf_cpu!=(int)cpu)bad=1;
        if(!mode) {
            uint64_t *report=cm_elf_pointer(p->elf,0x401000);
            if(report[0]!=1 || report[3]!=cpu || report[4]!=(cm_cpu_count()>=64?UINT64_MAX:(1ull<<cm_cpu_count())-1) || p->linux_enosys!=2 || !p->elf_preempts)bad=1;
            starts[cpu]=p->elf_start_tsc;ends[cpu]=p->elf_end_tsc;
        } else if(p->linux_enosys || cm_proc_children_pending(pid))bad=1;
        do {e=cm_proc_release_elf(pid);if(e==CM_ERR_BUSY)__asm__ volatile("pause");}while(e==CM_ERR_BUSY && cm_rdtsc()<deadline);
        if(e)bad=1;
    }
    if(bad)__atomic_add_fetch(&errors,1,__ATOMIC_RELAXED);
    kinfo("linux SMP AP lifecycle: cpu=%u mode=%s create/run/reap=%s",cpu,mode?"fork-COW-exec-wait":"FS-getcpu-affinity",bad?"FAIL":"ok");
}
#endif
int cm_linux_smp_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("linux SMP skipped: persona disabled native affinity unchanged");return 0;}
#ifdef CAPMIX_HOSTED
    kinfo("linux SMP hosted: AP hardware execution skipped");return 0;
#else
    if(cm_cpu_count()<2){kinfo("linux SMP skipped: only one CPU");return 0;}
    uint64_t before=cm_mm_free_pages()+cm_file_cache_pages(),charge,limit,after;
    cm_commit_stats(&charge,&limit);ready=errors=mode=0;memset(starts,0,sizeof(starts));memset(ends,0,sizeof(ends));
    cm_smp_sched_disable();
    if(cm_smp_buddy_test(worker)!=cm_cpu_count())return 1;
    unsigned first=cm_cpu_count()>2?1:0,n=cm_cpu_count()>2?2:1;
    uint64_t start=CM_MAX(starts[first],starts[n]),end=CM_MIN(ends[first],ends[n]);
    if(end<=start)errors++;
    kinfo("linux SMP overlap: cpus=%d pair=%u/%u overlap=%d getcpu/affinity/FS verified",cm_cpu_count(),first,n,end>start);
    mode=1;
    if(cm_smp_buddy_test(worker)!=cm_cpu_count())return 1;
    cm_commit_stats(&after,&limit);
    if(after!=charge || cm_mm_free_pages()+cm_file_cache_pages()!=before)errors++;
    kinfo("linux SMP selftest: %s (%u failures) COW/commit/pages/reap verified",errors?"FAIL":"PASS",errors);
    return (int)errors;
#endif
}
