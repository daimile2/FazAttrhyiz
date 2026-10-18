#include "capmix/abi.h"
#include "capmix/proc.h"
#include "capmix/linux_uapi.h"
#include "capmix/uaccess.h"
/* SIGCHLD-only coalesced pending bit. No user frames or rt_sigreturn. */
int64_t cm_linux_signal_call(uint32_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t size)
{
    cm_proc_t *p=cm_proc_current();
    if(!p || p->abi!=CM_ABI_LINUX)return -38;
    if(nr==127) { /* rt_sigpending(sigset*, kernel sigset size) */
        if(b!=8)return -22;
        uint64_t pending=__atomic_load_n(&cm_linux_group(p)->linux_sigpending,__ATOMIC_ACQUIRE)&
            __atomic_load_n(&p->linux_sigmask,__ATOMIC_ACQUIRE);
        return cm_copyout(p->elf,a,&pending,8)?-14:0;
    }
    if(size!=8)return -22;
    const uint64_t unblockable=(1ull<<8)|(1ull<<18); /* SIGKILL / SIGSTOP */
    if(nr==14) {
        uint64_t old=__atomic_load_n(&p->linux_sigmask,__ATOMIC_ACQUIRE),next=old,set;
        if(b) {
            if(a>2)return -22;
            if(cm_copyin(p->elf,&set,b,8))return -14;
            next=(a==0?old|set:a==1?old&~set:set)&~unblockable;
        }
        if(c && cm_copyout(p->elf,c,&old,8))return -14;
        __atomic_store_n(&p->linux_sigmask,next,__ATOMIC_RELEASE);
        /* Default action ignores an unblocked SIGCHLD; no handler to enter. */
        if(!(next&(1ull<<16)))__atomic_fetch_and(&cm_linux_group(p)->linux_sigpending,~(1ull<<16),__ATOMIC_ACQ_REL);
        return 0;
    }
    p=cm_linux_group(p);
    if(a<1 || a>64 || a==9 || a==19)return -22;
    uint64_t bit=1ull<<(a-1);
    cm_linux_sigaction_t old={.handler=!!(p->linux_sigignored&bit)},act;
    uint64_t policy=0;
    if(a==17) {
        int e=cm_linux_chld_policy(p,UINT64_MAX,&policy);if(e)return e;
        old.handler=policy&1;old.flags=policy&~1ull;
    }
    if(b) {
        if(cm_copyin(p->elf,&act,b,sizeof(act)))return -14;
        /* musl supplies SA_RESTORER even for DFL/IGN. Its pointer is never
         * called. Other flags, masks and handler addresses are unsupported. */
        if(act.handler>1 || (act.flags&~(0x04000000ull|(a==17?2ull:0ull))) || act.mask)return -95;
    }
    if(c && cm_copyout(p->elf,c,&old,sizeof(old)))return -14;
    if(b) {
        if(a==17) {
            if(p->linux_grouped)return -95;
            return cm_linux_chld_policy(p,act.handler|act.flags,&policy);
        }
        if(act.handler)p->linux_sigignored|=bit;else p->linux_sigignored&=~bit;
    }
    return 0;
}
