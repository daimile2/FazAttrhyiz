#include "capmix/linux_pipe.h"
#include "capmix/abi.h"
#include "capmix/linux_socket.h"
#include "capmix/config.h"
#include "capmix/boot.h"
#include "capmix/proc.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/uaccess.h"
#include "capmix/arch.h"
#include "capmix/sha256.h"
#include "capmix/numa.h"
#include "capmix/klog.h"
#include <string.h>
/* Offline package tools inspect this exact marker, never infer support from filenames. */
#if CM_LINUX_ABI
static const char package_support[] __attribute__((used))="CapMix.LinuxPersona.v1:compiled=1";
#else
static const char package_support[] __attribute__((used))="CapMix.LinuxPersona.v1:compiled=0";
#endif
int cm_linux_enabled(void)
{
#if CM_LINUX_ABI
    if(cm_boot_has_option("capmix.linux=0"))return 0;
    return cm_boot_has_option("capmix.linux=1") || cm_boot_is_selftest();
#else
    return 0;
#endif
}
/* Independent experimental gate; selftest alone does not enable L4a. */
int cm_linux_interp_enabled(void)
{
    return cm_linux_enabled() && !cm_boot_has_option("capmix.linux_interp=0") && cm_boot_has_option("capmix.linux_interp=1");
}
void cm_linux_random16(uint8_t out[16])
{
    uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    if(c&(1u<<30)) {
        uint64_t words[2];unsigned count=0;
        for(unsigned i=0;i<20 && count<2;i++) {
            unsigned char ok;uint64_t v;
            __asm__ volatile("rdrand %0; setc %1":"=r"(v),"=qm"(ok)::"cc");
            if(ok)words[count++]=v;
        }
        if(count==2){memcpy(out,words,16);return;}
    }
    static uint64_t serial;static unsigned warned;
    struct {uint64_t tsc,serial,address;} seed={cm_rdtsc(),__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED),(uintptr_t)out};
    uint8_t digest[32];(void)cm_sha256(&seed,sizeof(seed),digest);memcpy(out,digest,16);
    if(!__atomic_exchange_n(&warned,1,__ATOMIC_RELAXED))
        kinfo("linux AT_RANDOM: RDRAND unavailable; TSC/counter SHA256 fallback, not cryptographic entropy");
}
#if CM_LINUX_ABI
int64_t cm_linux_errno(int64_t e)
{
    switch(e) {
    case CM_ERR_INVAL:return -22;
    case CM_ERR_EXIST:return -17;
    case CM_ERR_NOTDIR:return -20;
    case CM_ERR_ISDIR:return -21;
    case CM_ERR_ROFS:return -30;
    case CM_ERR_NOTEMPTY:return -39;
    case CM_ERR_BUSY:return -16;
    case CM_ERR_DENIED:return -13;
    case CM_ERR_AGAIN:return -11;
    case CM_ERR_NOMEM:case CM_ERR_OVERCOMMIT:return -12;
    case CM_ERR_NOSPC:return -28;
    case CM_ERR_NOENT:return -2;
    case CM_ERR_NOTSUP:return -38;
    default:return e<0?-5:e;
    }
}
static int64_t l_write(uint64_t fd,uint64_t address,uint64_t n,uint64_t unused)
{
    (void)unused;cm_proc_t *p=cm_proc_current();
    if(fd>=CM_FD_MAX || p->fd[fd]<0 || (fd==0 && !(p->stdio_mask&1u)))return -9; /* EBADF */
    if(!cm_user_access(p->elf,address,n,0))return -14; /* EFAULT */
    if(cm_linux_pipe_is(p,fd)){int64_t r=cm_linux_pipe_io(p,fd,address,n,1);if(r>0)p->linux_write_bytes+=(uint64_t)r;return r;}
    if(cm_linux_socket_is(p,fd)) {
        int64_t r=cm_linux_socket_io(p,fd,address,n,1);if(r>0)p->linux_write_bytes+=(uint64_t)r;return r;
    }
    /* Preserve bound FILE/cap rights and byte accuracy, including binary output. */
    if(p->linux_console[fd]==1)return -9;
    int64_t result=p->linux_console[fd]?cm_native_console_write(address,n):cm_native_write(fd,address,n);
    if(result>0)p->linux_write_bytes+=(uint64_t)result;
    return cm_linux_errno(result);
}
static int64_t l_exit(uint64_t status,uint64_t b,uint64_t c,uint64_t d)
{(void)b;(void)c;(void)d;return cm_native_exit(status&255u);}
static int64_t l_arch_prctl(uint64_t code,uint64_t address,uint64_t c,uint64_t d)
{
    (void)c;(void)d;cm_proc_t *p=cm_proc_current();
    if(code==0x1002) { /* ARCH_SET_FS: no TLS allocator, only an address register. */
        if(address>=0x0000800000000000ull)return -1; /* EPERM: exclude kernel/noncanonical */
        cm_arch_user_fs_set(address);return 0;
    }
    if(code==0x1003) {
        uint64_t base=cm_arch_user_fs_get();
        return cm_copyout(p->elf,address,&base,sizeof(base))?-14:0;
    }
    return -22;
}
#endif
static int64_t dispatch(uint32_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if CM_LINUX_ABI
    cm_proc_t *active=cm_proc_current();
    cm_linux_child_events();
    if(nr==24){cm_sched_yield();cm_usermode_resume_entry();return 0;}
    if(nr==56)return cm_linux_clone(a,b,c,d,active->linux_regs.r8);
    if(nr==202)return cm_linux_futex(a,b,c,d);
    /* Threaded groups allow only bounded FILE/console dup/close. Mapping,
     * fd creation, fcntl, pipe and socket mutations remain frozen. */
    if(cm_linux_group(active)->linux_grouped) {
        if(nr==2 || nr==257 || nr==22 || nr==293 || nr==72 || nr==57 || nr==59 || nr==61 ||
           (nr>=41 && nr<=55) || nr==288 || nr==10 || nr==11 || nr==9 || nr==12)return -95;
        if((nr==0 || nr==1 || nr==19 || nr==20) && cm_linux_socket_is(active,a))return -95;
    }
    static const struct {uint32_t nr;int64_t (*fn)(uint64_t,uint64_t,uint64_t,uint64_t);} calls[]={
        {1,l_write},{60,l_exit},{158,l_arch_prctl}
    };
    for(unsigned i=0;i<sizeof(calls)/sizeof(calls[0]);i++)if(calls[i].nr==nr)return calls[i].fn(a,b,c,d);
    if(nr==309) { /* getcpu(cpu*, node*, unused cache): validate before writing */
        uint32_t cpu=cm_cpu_id(),node=(uint32_t)cm_numa_cpu_node((int)cpu);
        if((a && !cm_user_access(active->elf,a,4,1)) || (b && !cm_user_access(active->elf,b,4,1)))return -14;
        if(a && cm_copyout(active->elf,a,&cpu,4))return -14;
        if(b && cm_copyout(active->elf,b,&node,4))return -14;
        return 0;
    }
    if(nr==203)return cm_linux_setaffinity(a,b,c);
    if(nr==204) { /* eligible CPU set; singleton migration uses the separate 203 safe point */
        if(a && a!=active->pid && a!=cm_linux_group(active)->pid)return -3;
        if(b<8)return -22;
        uint64_t mask=cm_cpu_count()>=64?UINT64_MAX:(1ull<<cm_cpu_count())-1;
        return cm_copyout(active->elf,c,&mask,8)?-14:8;
    }
    if(nr==13 || nr==14 || nr==127)return cm_linux_signal_call(nr,a,b,c,d);
    if(nr==22 || nr==293)return cm_linux_pipe_create(active,a,nr==293?b:0);
    if(nr==57)return cm_linux_fork();
    if(nr==61)return cm_linux_wait4((int64_t)a,b,c,d);
    if(nr==59)return cm_linux_execve(a,b,c);
    if(nr==12)return (int64_t)cm_elf_brk(active->elf,a);
    if(nr==9) {
        uint64_t fd=active->linux_regs.r8,offset=active->linux_regs.r9;
        if(d&0x20)return cm_elf_mmap(active->elf,a,b,c,d,fd,offset);
        if(d!=2 || c!=1)return -95;
        if(fd>=CM_FD_MAX || active->fd[fd]<0 || active->linux_console[fd])return -9;
        cm_object_t *o=NULL;
        if(cm_native_file_acquire((int)fd,CM_RIGHT_READ,&o))return -9;
        int64_t r=cm_elf_mmap_file(active->elf,a,b,c,d,o->payload,offset);
        cm_obj_release(o);return r;
    }
    if(nr==10 && active && active->elf && active->elf->rtld)return cm_elf_mprotect(active->elf,a,b,c);
    if(nr==11)return cm_elf_munmap(active->elf,a,b);
    int64_t result;if(cm_linux_socket_call(nr,a,b,c,d,&result))return result;
    if(cm_linux_file_call(nr,a,b,c,d,&result))return result;
#else
    (void)nr;(void)a;(void)b;(void)c;(void)d;
#endif
    return -38;
}

/* Count final syscall results once, including errors returned by socket/file
 * helpers. Fixed storage: 512 numbers and an explicit out-of-range bucket.
 * Atomics keep snapshots safe across CPUs; snapshots are not a global barrier. */
static uint64_t enosys[CM_LINUX_ENOSYS_BUCKETS];
/* No spinlock spans I/O or a scheduler switch. A competing member gets
 * EAGAIN, while the owning continuation retains the gate until return. Pipe
 * I/O must not hold it: its peer may need to run to unblock it. Pipe/socket
 * slots cannot be replaced/closed by the grouped mutation whitelist. */
#if CM_LINUX_ABI
static int file_gate_needed(cm_proc_t *p,uint32_t nr,uint64_t fd)
{
    if(!p || !cm_linux_group(p)->linux_grouped)return 0;
    switch(nr) {
    case 0:case 1:case 19:case 20:
        if(cm_linux_pipe_is(p,fd))return 0;
        return 1;
    case 3:case 5:case 8:case 16:case 32:case 33:case 217:case 262:return 1;
    default:return 0;
    }
}
#endif
int64_t cm_linux_dispatch(uint32_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if CM_LINUX_ABI
    cm_proc_t *owner=cm_proc_current(),*gate=NULL;
    if(owner){owner->linux_last_syscall=nr;owner->linux_calls++;}
    if(file_gate_needed(owner,nr,a)) {
        gate=cm_linux_group(owner);uint64_t empty=0;
        if(!__atomic_compare_exchange_n(&gate->linux_fd_owner,&empty,owner->pid,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED) && empty!=owner->pid)
            return -11;
        gate->linux_fd_depth++;
    }
#endif
    int64_t result=dispatch(nr,a,b,c,d);
#if CM_LINUX_ABI
    if(gate && !--gate->linux_fd_depth)__atomic_store_n(&gate->linux_fd_owner,0,__ATOMIC_RELEASE);
#endif
    cm_proc_t *p=cm_proc_current();
    if(result==-38 && p && p->abi==CM_ABI_LINUX) {
        __atomic_fetch_add(&enosys[nr<512?nr:512],1,__ATOMIC_RELAXED);
        p->linux_enosys++;
        if(p->linux_enosys<=8)kinfo("linux ENOSYS: pid=%llu nr=%u count=%llu",(unsigned long)p->pid,nr,(unsigned long)p->linux_enosys);
    }
    return result;
}
void cm_linux_enosys_snapshot(uint64_t out[CM_LINUX_ENOSYS_BUCKETS])
{for(unsigned i=0;i<CM_LINUX_ENOSYS_BUCKETS;i++)out[i]=__atomic_load_n(&enosys[i],__ATOMIC_RELAXED);}
void cm_linux_enosys_dump(const char *scope,const uint64_t *before)
{
    uint64_t total=0;
    for(unsigned i=0;i<CM_LINUX_ENOSYS_BUCKETS;i++) {
        uint64_t n=__atomic_load_n(&enosys[i],__ATOMIC_RELAXED)-(before?before[i]:0);total+=n;
        if(n)kinfo("linux ENOSYS histogram: scope=%s nr=%u count=%llu%s",scope,i,(unsigned long)n,i==512?" overflow=1":"");
    }
    kinfo("linux ENOSYS histogram end: scope=%s total=%llu buckets=512 overflow=explicit",scope,(unsigned long)total);
}
