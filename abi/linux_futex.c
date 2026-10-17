#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/linux_uapi.h"
/* Key is (address-space leader, user VA), never a host pointer. Bounded by
 * proc execution slots. rq prepare + enrollment are atomic under this lock. */
typedef struct {cm_proc_t *p,*group;uint64_t va,end;int result;} wait_t;
static wait_t waits[CM_MAX_PROCS];
static unsigned busy,active;
static uint64_t lock(void){uint64_t f=cm_irq_save();while(__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return f;}
static void unlock(uint64_t f){__atomic_store_n(&busy,0,__ATOMIC_RELEASE);cm_irq_restore(f);}
static void finish(wait_t *w,int result)
{cm_thread_t *th=w->p->main;w->result=result;w->group=NULL;__atomic_sub_fetch(&active,1,__ATOMIC_RELEASE);cm_thread_wake(th);}
void cm_linux_futex_poll(void)
{
    if(!__atomic_load_n(&active,__ATOMIC_ACQUIRE))return;
    uint64_t f=lock(),now=cm_rdtsc();
    for(unsigned i=0;i<CM_MAX_PROCS;i++)if(waits[i].group) {
        wait_t *w=&waits[i];
        if(__atomic_load_n(&w->p->kill_pending,__ATOMIC_ACQUIRE))finish(w,-4);
        else if(w->end && now>=w->end)finish(w,-110);
    }
    unlock(f);
}
void cm_linux_futex_cancel(cm_proc_t *p)
{uint64_t f=lock();for(unsigned i=0;i<CM_MAX_PROCS;i++)if(waits[i].p==p){if(waits[i].group)__atomic_sub_fetch(&active,1,__ATOMIC_RELEASE);waits[i].group=NULL;waits[i].p=NULL;}unlock(f);}
int64_t cm_linux_futex_wake(cm_proc_t *p,uint64_t va,unsigned n)
{
    cm_proc_t *g=cm_linux_group(p);unsigned count=0;uint64_t f=lock();
    for(unsigned i=0;i<CM_MAX_PROCS && count<n;i++)if(waits[i].group==g && waits[i].va==va){finish(&waits[i],0);count++;}
    unlock(f);return count;
}
int64_t cm_linux_futex(uint64_t va,uint64_t op,uint64_t value,uint64_t timeout)
{
    cm_proc_t *p=cm_proc_current();unsigned cmd=(unsigned)op&~128u;
    if(op!=cmd && op!=(cmd|128u))return -38;
    if(cmd!=0 && cmd!=1)return -38;
    if(va&3)return -22;
    uint32_t observed;if(cm_copyin(p->elf,&observed,va,4))return -14;
    if(cmd==1){if((int32_t)value<0)return -22;return cm_linux_futex_wake(p,va,(unsigned)value);}
    uint64_t end=0;
    if(timeout) {
        cm_linux_timespec_t ts;if(cm_copyin(p->elf,&ts,timeout,sizeof(ts)))return -14;
        if(ts.sec<0 || ts.sec>60 || ts.nsec<0 || ts.nsec>=1000000000)return -22;
        uint64_t hz=cm_tsc_hz();end=cm_rdtsc()+hz*(uint64_t)ts.sec+hz/1000000000*(uint64_t)ts.nsec+(hz%1000000000)*(uint64_t)ts.nsec/1000000000;
    }
    uint64_t f=lock();
    if(cm_copyin(p->elf,&observed,va,4)){unlock(f);return -14;}
    if(observed!=(uint32_t)value){unlock(f);return -11;}
    if(end && cm_rdtsc()>=end){unlock(f);return -110;}
#ifdef CAPMIX_HOSTED
    unlock(f);return -11; /* no hosted context switch into ring3 */
#else
    wait_t *w=&waits[p->elf_entry_slot];*w=(wait_t){p,cm_linux_group(p),va,end,0};
    __atomic_add_fetch(&active,1,__ATOMIC_RELEASE);
    cm_thread_block_prepare(p->main,NULL);
    unlock(f);
    kinfo("linux futex wait: tid=%llu blocked=1",(unsigned long)p->pid);
    cm_sched_yield();cm_usermode_resume_entry();
    f=lock();int result=w->result;if(w->group)__atomic_sub_fetch(&active,1,__ATOMIC_RELEASE);w->p=NULL;w->group=NULL;unlock(f);
    kinfo("linux futex resume: tid=%llu result=%d",(unsigned long)p->pid,result);
    return result;
#endif
}
