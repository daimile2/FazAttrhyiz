#include "capmix/linux_pipe.h"
#include "capmix/proc.h"
#include "capmix/uaccess.h"
#include "capmix/elf.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include <string.h>
/* Eight resident pipes, 2048 bytes each, atomic writes up to 512 bytes.
 * Each endpoint is a capability; DUP/fork share its open-description lifetime.
 * Waiting yields/rechecks (kill interruptible), not a new scheduler wait queue. */
#define PIPES 8u
#define CAPACITY 2048u
#define ATOMIC 512u
#define NONBLOCK 0x800u
#define CLOEXEC 0x80000u
typedef struct pipe pipe_t;
typedef struct {pipe_t *pipe;unsigned write,nonblock;} endpoint_t;
struct pipe {unsigned slot,readers,writers,head,count;endpoint_t ends[2];uint8_t data[CAPACITY];};
_Static_assert(sizeof(pipe_t)<=CM_PAGE_SIZE,"pipe metadata/data page");
static unsigned lock_word,slots;
static uint64_t lock(void){uint64_t f=cm_irq_save();while(__atomic_exchange_n(&lock_word,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return f;}
static void unlock(uint64_t f){__atomic_store_n(&lock_word,0,__ATOMIC_RELEASE);cm_irq_restore(f);}
unsigned cm_linux_pipe_live(void){uint64_t f=lock();unsigned n=0;for(unsigned i=0;i<PIPES;i++)n+=(slots>>i)&1u;unlock(f);return n;}
static void drop_end(void *payload)
{
    endpoint_t *e=payload;pipe_t *q=e->pipe;uint64_t f=lock();
    if(e->write)q->writers=0;else q->readers=0;
    int last=!q->readers && !q->writers;if(last)slots&=~(1u<<q->slot);
    unlock(f);if(last)cm_page_free(q,0);
}
static int get(cm_proc_t *p,uint64_t fd,cm_rights_t rights,cm_object_t **o)
{
    if(!p || fd>=CM_FD_MAX || p->fd[fd]<0 || p->linux_console[fd])return -9;
    return cm_handle_acquire(p->task,p->fd[fd],CM_OBJ_LINUX_PIPE,rights,o)?-9:0;
}
int cm_linux_pipe_is(cm_proc_t *p,uint64_t fd)
{cm_object_t *o=NULL;if(get(p,fd,0,&o))return 0;cm_obj_release(o);return 1;}
int64_t cm_linux_pipe_create(cm_proc_t *p,uint64_t address,uint64_t flags)
{
    if(flags&~(NONBLOCK|CLOEXEC))return -22;
    if(!cm_user_access(p->elf,address,8,1))return -14;
    int fds[2],found=0;
    for(int i=0;i<CM_FD_MAX && found<2;i++)if(p->fd[i]<0)fds[found++]=i;
    if(found<2)return -24;
    uint64_t irq=lock();unsigned slot;
    for(slot=0;slot<PIPES;slot++)if(!(slots&(1u<<slot)))break;
    if(slot==PIPES){unlock(irq);return -23;}slots|=1u<<slot;unlock(irq);
    pipe_t *q=cm_page_alloc(0);if(!q){irq=lock();slots&=~(1u<<slot);unlock(irq);return -12;}
    memset(q,0,sizeof(*q));q->slot=slot;q->readers=q->writers=1;
    cm_object_t *o[2]={NULL,NULL};int handles[2]={-1,-1},error=-12;
    for(unsigned i=0;i<2;i++) {
        q->ends[i]=(endpoint_t){q,i,!!(flags&NONBLOCK)};
        o[i]=cm_obj_mint_owned(CM_OBJ_LINUX_PIPE,&q->ends[i],(i?CM_RIGHT_WRITE:CM_RIGHT_READ)|CM_RIGHT_DUPLICATE,drop_end);
        if(!o[i])goto fail;
        handles[i]=cm_handle_install(p->task,o[i],o[i]->default_rights);if(handles[i]<0){error=-24;goto fail;}
    }
    if(cm_copyout(p->elf,address,fds,8)){error=-14;goto fail;}
    for(unsigned i=0;i<2;i++){p->fd[fds[i]]=handles[i];p->linux_console[fds[i]]=0;p->linux_cloexec[fds[i]]=!!(flags&CLOEXEC);if(fds[i]<3)p->stdio_mask|=1u<<fds[i];cm_obj_release(o[i]);}
    return 0;
fail:
    /* Mark endpoints never minted as closed before releasing the live ones. */
    irq=lock();if(!o[0])q->readers=0;if(!o[1])q->writers=0;unlock(irq);
    int any=o[0]||o[1];
    for(unsigned i=0;i<2;i++){if(handles[i]>=0)(void)cm_handle_drop(p->task,handles[i]);if(o[i])cm_obj_release(o[i]);}
    if(!any){irq=lock();slots&=~(1u<<slot);unlock(irq);cm_page_free(q,0);}
    return error;
}
int64_t cm_linux_pipe_io(cm_proc_t *p,uint64_t fd,uint64_t address,uint64_t n,int write)
{
    cm_object_t *o;int error=get(p,fd,write?CM_RIGHT_WRITE:CM_RIGHT_READ,&o);if(error)return error;
    if(!cm_user_access(p->elf,address,n,!write)){cm_obj_release(o);return -14;}
    endpoint_t *e=o->payload;pipe_t *q=e->pipe;int64_t done=0;uint8_t temp[ATOMIC];
    while((uint64_t)done<n) {
        unsigned want=(unsigned)CM_MIN(n-(uint64_t)done,ATOMIC);
        if(write && cm_copyin(p->elf,temp,address+done,want)){error=-14;break;}
        uint64_t f=lock();
        if(write && !q->readers){unlock(f);error=-32;break;} /* EPIPE; no SIGPIPE delivery */
        unsigned available=write?CAPACITY-q->count:q->count;
        if((write && available>=want) || (!write && available)) {
            if(!write){want=CM_MIN(want,available);for(unsigned i=0;i<want;i++)temp[i]=q->data[(q->head+i)%CAPACITY];
                if(cm_copyout(p->elf,address+done,temp,want)){unlock(f);error=-14;break;}
                q->head=(q->head+want)%CAPACITY;q->count-=want;
            }else {for(unsigned i=0;i<want;i++)q->data[(q->head+q->count+i)%CAPACITY]=temp[i];q->count+=want;}
            unlock(f);done+=want;if(!write)break;continue;
        }
        int eof=!write && !q->writers,nonblock=e->nonblock;unlock(f);
        if(eof)break;
        if(nonblock){error=-11;break;}
        if(__atomic_load_n(&p->kill_pending,__ATOMIC_ACQUIRE)){error=-4;break;}
#ifdef CAPMIX_HOSTED
        error=-11;break; /* hosted cannot switch into ring3 */
#else
        cm_sched_yield();cm_usermode_resume_entry();
#endif
    }
    cm_obj_release(o);return done?done:error;
}
int64_t cm_linux_pipe_dup(cm_proc_t *p,uint64_t old,uint64_t target)
{
    cm_object_t *o;int e=get(p,old,CM_RIGHT_DUPLICATE,&o);if(e)return e;cm_obj_release(o);
    if(target>=CM_FD_MAX)return -9;
    if(old==target)return target;
    int h;e=cm_handle_duplicate(p->task,p->fd[old],p->task,&h,CM_RIGHT_ALL);if(e)return -24;
    if(p->fd[target]>=0){int64_t ignored;(void)cm_linux_file_call(3,target,0,0,0,&ignored);}
    p->fd[target]=h;p->linux_console[target]=0;p->linux_cloexec[target]=0;if(target<3)p->stdio_mask|=1u<<target;return target;
}
int64_t cm_linux_pipe_fcntl(cm_proc_t *p,uint64_t fd,uint64_t op,uint64_t arg)
{
    cm_object_t *o;int e=get(p,fd,0,&o);if(e)return e;endpoint_t *end=o->payload;uint64_t f=lock();
    if(op==3)e=(end->write?1:0)|(end->nonblock?NONBLOCK:0);
    else if(op==4){if(arg&~(3u|NONBLOCK))e=-22;else {end->nonblock=!!(arg&NONBLOCK);e=0;}}
    else e=-22;
    unlock(f);cm_obj_release(o);return e;
}
