#include "capmix/abi.h"
#include "capmix/linux_socket.h"
#include "capmix/config.h"
#include "capmix/boot.h"
#include "capmix/proc.h"
#include "capmix/elf.h"
#include "capmix/uaccess.h"
#include "capmix/arch.h"
#include "capmix/sha256.h"
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
int64_t cm_linux_dispatch(uint32_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if CM_LINUX_ABI
    cm_proc_t *active=cm_proc_current();if(active){active->linux_last_syscall=nr;active->linux_calls++;}
    static const struct {uint32_t nr;int64_t (*fn)(uint64_t,uint64_t,uint64_t,uint64_t);} calls[]={
        {1,l_write},{60,l_exit},{158,l_arch_prctl}
    };
    for(unsigned i=0;i<sizeof(calls)/sizeof(calls[0]);i++)if(calls[i].nr==nr)return calls[i].fn(a,b,c,d);
    if(nr==57)return cm_linux_fork();
    if(nr==61)return cm_linux_wait4((int64_t)a,b,c,d);
    if(nr==59)return cm_linux_execve(a,b,c);
    if(nr==12)return (int64_t)cm_elf_brk(active->elf,a);
    if(nr==9)return -95; /* mmap unsupported: including file/MAP_SHARED */
    int64_t result;if(cm_linux_socket_call(nr,a,b,c,d,&result))return result;
    if(cm_linux_file_call(nr,a,b,c,d,&result))return result;
#else
    (void)nr;(void)a;(void)b;(void)c;(void)d;
#endif
    cm_proc_t *p=cm_proc_current();if(p)p->linux_enosys++;
    return -38;
}
