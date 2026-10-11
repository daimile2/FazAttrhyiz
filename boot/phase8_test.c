#include "capmix/trace.h"
#include "capmix/obs.h"
#include "capmix/ipc.h"
#include "capmix/sched.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>
#include "capmix/fs.h"
#include "capmix/proc.h"
#include "capmix/elf.h"
#include "../build/trace_elf.inc"
static unsigned per_cpu_count[CM_MAX_CPUS];
static void concurrent(unsigned cpu)
{
    unsigned n=per_cpu_count[cpu]++;
    unsigned tp=cpu%4==0 ? CM_TP_SCHED_SWITCH : cpu%4==1 ? CM_TP_IPC_SEND : cpu%4==2 ? CM_TP_UDP_TX : CM_TP_IRQ_DELIVER;
    cm_tp_emit(tp,0x12340001,cpu,n,0xcafe);
}
#define VERIFY(x) do { if (!(x)) { kerr("phase8: check failed line=%u",(unsigned)__LINE__); fails++; } } while (0)
static int user_read_test(cm_task_t *admin)
{
    int fails=0; cm_file_t *f=NULL;
    cm_error_t open_error=cm_vfs_open("/bin/trace.elf",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&f);
    if (open_error!=CM_OK) { kerr("phase8: trace ELF open error=%d",open_error); return 1; }
    VERIFY(cm_vfs_write(f,cm_trace_elf,sizeof(cm_trace_elf))==(int64_t)sizeof(cm_trace_elf));
    VERIFY(cm_vfs_fsync(f)==CM_OK); cm_vfs_close(f);
    cm_spawn_attr_t attr={.path="/bin/trace.elf",.qos=CM_QOS_USER_INTERACTIVE}; cm_id_t pid=0;
    cm_error_t spawn_error=cm_posix_spawn(&attr,&pid);
    if (spawn_error!=CM_OK) { kerr("phase8: trace ELF spawn error=%d",spawn_error); return fails+1; }
    cm_proc_t *p=cm_proc_lookup(pid);
    if (!p || !p->elf) return fails+1;
    int h=cm_trace_open(admin,CM_TP_BIT(CM_TP_IPC_SEND),0,1),good=-1,bad=-1;
    if (h<0) { kerr("phase8: trace ELF cap create error=%d",h); return fails+1; }
    VERIFY(cm_handle_duplicate(admin,h,p->task,&good,CM_RIGHT_TRACE_READ)==CM_OK);
    VERIFY(cm_handle_duplicate(admin,h,p->task,&bad,CM_RIGHT_WRITE)==CM_OK);
    uint64_t *parameters=cm_elf_pointer(p->elf,0x401000);
    if (!parameters) return fails+1;
    parameters[0]=(uint64_t)good; parameters[1]=(uint64_t)bad; parameters[2]=cm_trace_head(0);
    VERIFY(cm_trace_configure(admin,h,CM_TP_BIT(CM_TP_IPC_SEND))==CM_OK);
    TRACE_IPC_SEND(cm_trace_new(),0x88,0,0);
    cm_sched_run_until_idle(64);
#ifdef CAPMIX_HOSTED
    VERIFY(p->exited && p->exit_code==CM_ERR_NOTSUP);
    cm_trace_process_exit(p->task);
    kinfo("phase8: user trace ELF syscall skipped on hosted; capability API checks verified");
#else
    VERIFY(p->exited && p->exit_code==0 && parameters[17]==3 && p->elf_written_bytes==17);
    if (p->exit_code) kerr("phase8: trace ELF failed stage=%llu result=%lld tp=%llu a0=%llx",
        (unsigned long)parameters[15],(long)parameters[16],(unsigned long)parameters[11],(unsigned long)parameters[12]);
    else kinfo("phase8: user syscall cap granted/denied verified CPL=3 trace user rx ok exit=0");
#endif
    VERIFY(cm_trace_configure(admin,h,0)==CM_OK); cm_handle_drop(admin,h);
    if (p->exited) VERIFY(cm_proc_reap_elf(pid)==CM_OK);
    return fails;
}
int cm_phase8_selftest(void)
{
    int fails=0;
    cm_task_t *t=cm_task_create("trace-reader",CM_QOS_DEFAULT);
    if (!t) return 1;
    int h=cm_trace_open(t,CM_TP_ALL,0,(1ull<<CM_MAX_CPUS)-1),denied=-1,readonly=-1;
    if (h<0) return 1;
    VERIFY(cm_handle_duplicate(t,h,t,&denied,CM_RIGHT_WRITE)==CM_OK);
    VERIFY(cm_handle_duplicate(t,h,t,&readonly,CM_RIGHT_TRACE_READ)==CM_OK);
    cm_trace_read_t read={0};
    VERIFY(cm_trace_read(t,denied,0,&read)==CM_ERR_DENIED);
    VERIFY(cm_trace_configure(t,readonly,CM_TP_ALL)==CM_ERR_DENIED);
    kinfo("phase8: cap denied verified (TRACE_READ missing / configure read-only)");
    int a,b; VERIFY(cm_channel_create(t,&a,&b)==CM_OK);
    uint64_t base=cm_trace_head(0),last_tsc=0;
    VERIFY(cm_trace_configure(t,h,CM_TP_BIT(CM_TP_SCHED_SWITCH)|CM_TP_BIT(CM_TP_IPC_SEND)|CM_TP_BIT(CM_TP_IPC_RECV))==CM_OK);
    cm_trace_t root=cm_trace_new();
    for (unsigned n=0;n<16;n++) TRACE_SCHED_SWITCH(root,n,n+1,0);
    cm_trace_t traces[8];
    for (unsigned n=0;n<8;n++) {
        cm_ipc_msg_t msg={.trace=n ? cm_trace_child(traces[n-1]) : root,.opcode=n},reply;
        traces[n]=msg.trace;
        VERIFY(cm_ipc_send_deferred(t,a,&msg)==CM_OK);
        VERIFY(cm_ipc_try_recv(t,b,&reply)==CM_OK && reply.trace==msg.trace);
    }
    read=(cm_trace_read_t){.cursor=base};
    unsigned got=0;
    while (cm_trace_read(t,readonly,0,&read)==CM_OK) {
        cm_trace_record_t *e=&read.event;
        VERIFY(e->seq==base+got+1 && e->cpu==0 && e->tsc>=last_tsc);
        last_tsc=e->tsc;
        if (got<16) VERIFY(e->tp==CM_TP_SCHED_SWITCH && e->a0==got && e->trace==root);
        else {
            unsigned n=(got-16)/2;
            VERIFY(n<8 && e->tp==((got&1) ? CM_TP_IPC_RECV : CM_TP_IPC_SEND));
            if (n<8) VERIFY(e->trace==traces[n] && e->a1==n && (e->trace>>16)==(root>>16) &&
                            e->parent==(n ? traces[n-1] : 0));
        }
        got++;
    }
    VERIFY(got==32 && !read.lost);
    kinfo("phase8: trace injected=32 readback=%u verified (16 switch + 8 channel send/recv causal chain)",got);
    kinfo("phase8: cap granted verified (read-only TRACE capability)");
    VERIFY(cm_trace_configure(t,h,0)==CM_OK);
    uint64_t start=cm_rdtsc(),before=cm_trace_head(0); unsigned side_effect=0;
    for (unsigned n=0;n<10000;n++) TRACE_SCHED_SWITCH(root,side_effect++,0,0);
    uint64_t elapsed=cm_rdtsc()-start;
    VERIFY(!side_effect && before==cm_trace_head(0));
    kinfo("phase8: mask disabled verified injected=10000 recorded=0 cycles=%llu",(unsigned long)elapsed);
    VERIFY(cm_trace_configure(t,h,CM_TP_ALL)==CM_OK);
    uint64_t heads[CM_MAX_CPUS];
    for (unsigned cpu=0;cpu<CM_MAX_CPUS;cpu++) { heads[cpu]=cm_trace_head(cpu); per_cpu_count[cpu]=0; }
    int cpus=cm_smp_trace_test(concurrent,64);
    VERIFY(cpus>0);
    unsigned total=0;
    for (int cpu=0;cpu<cpus;cpu++) {
        read=(cm_trace_read_t){.cursor=heads[cpu]}; unsigned n=0; last_tsc=0;
        while (cm_trace_read(t,h,(unsigned)cpu,&read)==CM_OK) {
            cm_trace_record_t *e=&read.event;
            VERIFY(e->cpu==(unsigned)cpu && e->seq==heads[cpu]+n+1 && e->a0==(unsigned)cpu &&
                   e->a1==n && e->a2==0xcafe && e->tsc>=last_tsc);
            last_tsc=e->tsc; n++;
        }
        VERIFY(n==64 && !read.lost); total+=n;
    }
    VERIFY(cpus>0 && total==(unsigned)cpus*64);
    kinfo("phase8: per-cpu concurrent verified cpus=%d injected=%u readback=%u ordered isolated",cpus,cpus>0 ? (unsigned)cpus*64 : 0,total);
    VERIFY(cm_trace_configure(t,h,CM_TP_BIT(CM_TP_SCHED_SWITCH))==CM_OK);
    before=cm_trace_head(0);
    for (unsigned n=0;n<CM_TP_RING_SIZE+8;n++) TRACE_SCHED_SWITCH(root,n,0,0);
    read=(cm_trace_read_t){.cursor=before}; got=0;
    while (cm_trace_read(t,h,0,&read)==CM_OK) { VERIFY(read.event.a0==got+8); got++; }
    VERIFY(got==CM_TP_RING_SIZE && read.lost==8);
    kinfo("phase8: overwrite allowed verified retained=%u lost=%llu",got,(unsigned long)read.lost);
    int scoped=cm_trace_open(t,CM_TP_BIT(CM_TP_SCHED_SWITCH),UINT64_MAX,1);
    VERIFY(scoped>0);
    read=(cm_trace_read_t){.cursor=before};
    VERIFY(cm_trace_read(t,scoped,0,&read)==CM_ERR_AGAIN);
    VERIFY(cm_trace_read(t,scoped,1,&read)==CM_ERR_DENIED);
    VERIFY(cm_trace_configure(t,scoped,CM_TP_ALL)==CM_ERR_DENIED);
    if (scoped>0) cm_handle_drop(t,scoped);
    VERIFY(cm_trace_configure(t,h,0)==CM_OK);
    cm_handle_drop(t,a); cm_handle_drop(t,b);
    cm_handle_drop(t,denied); cm_handle_drop(t,readonly); cm_handle_drop(t,h);
    VERIFY(__atomic_load_n(&cm_tp_enabled_mask,__ATOMIC_ACQUIRE)==0);
    fails+=user_read_test(t);
    kinfo("phase8 selftest: %s (%d failures)",fails ? "FAIL" : "PASS",fails);
    return fails;
}
