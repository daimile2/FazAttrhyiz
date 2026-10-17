#include "capmix/linux_socket.h"
#include "capmix/linux_uapi.h"
#include "capmix/proc.h"
#include "capmix/uaccess.h"
#include "capmix/abi.h"
#include "capmix/tcp.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/cap_internal.h"
#include "capmix/klog.h"
#include <string.h>
#define NONBLOCK 0x800u
#define CLOEXEC 0x80000u
#define WAIT_MS 3000u
#define CLOSE_MS 1000u
#define MSG_DONTWAIT 0x40u
#define MSG_NOSIGNAL 0x4000u
/* Kernel-only handle table, not a registered/scheduled task. Backend APIs
 * accept capability spaces; no task id or user authority is fabricated.
 * Keeping raw PORTs here prevents process_exit from aborting another alias. */
typedef struct {
    cm_task_t caps;
    int hid,kind,ifid,bound,connected,listening,nonblock,busy,counted;
    uint32_t remote,local;
    uint16_t dport,sport;
} linux_socket_t;
_Static_assert(sizeof(linux_socket_t)<=2*CM_PAGE_SIZE,"socket plus private capspace");
static unsigned live;
static unsigned udp_next;
unsigned cm_linux_socket_live(void){return __atomic_load_n(&live,__ATOMIC_RELAXED);}
static int unsupported(void){return -38;} /* final dispatch accounts once */
int cm_linux_socket_is(cm_proc_t *p,uint64_t fd)
{
    if(!p || p->abi!=CM_ABI_LINUX || fd>=CM_FD_MAX || p->fd[fd]<0 || p->linux_console[fd])return 0;
    uint64_t f=cm_cap_lock();cm_handle_t *h=cm_handle_lookup(p->task,p->fd[fd]);
    int yes=h && h->obj->type==CM_OBJ_LINUX_SOCKET;cm_cap_unlock(f);return yes;
}
static int get(cm_proc_t *p,uint64_t fd,cm_rights_t need,cm_object_t **out)
{
    if(fd>=CM_FD_MAX || p->fd[fd]<0)return -9;
    if(!cm_linux_socket_is(p,fd))return -88;
    int e=cm_handle_acquire(p->task,p->fd[fd],CM_OBJ_LINUX_SOCKET,need,out);return e==CM_ERR_DENIED?-13:e?-9:0;
}
static int neterr(int e)
{
    if(e>=0)return e;
    switch(e){case CM_ERR_AGAIN:return -11;case CM_ERR_NOMEM:return -12;case CM_ERR_DENIED:return -13;
    case CM_ERR_NOSPC:return -105;case CM_ERR_EXIST:return -98;case CM_ERR_BUSY:return -114;
    case CM_ERR_NOENT:return -101;case CM_ERR_DEAD:return -104;case CM_ERR_NOTSUP:return unsupported();default:return -22;}
}
static uint64_t deadline(void){return cm_rdtsc()+cm_tsc_hz()/1000*WAIT_MS;}
static int turn(cm_proc_t *p,uint64_t end)
{
    if(__atomic_load_n(&p->kill_pending,__ATOMIC_ACQUIRE))return -4;
    if(cm_rdtsc()>=end)return -11;
    cm_sched_yield();cm_usermode_resume_entry();return 0;
}
static void destroy(void *payload)
{
    linux_socket_t *s=payload;
    if(s->hid>0) {
        int e=s->kind==1?cm_tcp_close(&s->caps,s->hid,CLOSE_MS):cm_udp_close(&s->caps,s->hid);
        if(e)kinfo("linux L3 socket final close: transport=%d handle released/abort fallback",e);
    }
    /* Also covers failed open/accept or a BUSY early return from close. */
    cm_tcp_process_exit(&s->caps);cm_udp_process_exit(&s->caps);
    if(s->counted)__atomic_sub_fetch(&live,1,__ATOMIC_RELAXED);
    cm_page_free(s,1);
}
static linux_socket_t *allocate(int kind)
{
    linux_socket_t *s=cm_page_alloc(1);if(!s)return NULL;
    memset(s,0,2*CM_PAGE_SIZE);s->kind=kind;s->hid=-1;s->ifid=-1;s->caps.name="linux-socket-private";return s;
}
static int room(cm_proc_t *p){for(int fd=0;fd<CM_FD_MAX;fd++)if(p->fd[fd]<0)return fd;return -1;}
static int publish(cm_proc_t *p,linux_socket_t *s,unsigned flags)
{
    int fd=room(p);if(fd<0){destroy(s);return -24;}
    cm_rights_t rights=CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE;
    cm_object_t *o=cm_obj_mint_owned(CM_OBJ_LINUX_SOCKET,s,rights,destroy);
    if(!o){destroy(s);return -105;}
    s->counted=1;__atomic_add_fetch(&live,1,__ATOMIC_RELAXED);
    int hid=cm_handle_install(p->task,o,rights);cm_obj_release(o);if(hid<0)return -24;
    p->fd[fd]=hid;p->linux_console[fd]=0;p->linux_cloexec[fd]=!!(flags&CLOEXEC);
    if(fd<3)p->stdio_mask|=1u<<fd;
    return fd;
}
static int socket_new(cm_proc_t *p,uint64_t domain,uint64_t type,uint64_t proto)
{
    if(domain!=2)return -97;
    unsigned kind=(unsigned)type&15u;if(type&~(15u|NONBLOCK|CLOEXEC))return unsupported();
    if(kind!=1 && kind!=2)return unsupported();
    if(proto && proto!=(kind==1?6u:17u))return -93;
    if(room(p)<0)return -24;
    linux_socket_t *s=allocate((int)kind);if(!s)return -12;s->nonblock=!!(type&NONBLOCK);
    int e=kind==1?cm_tcp_open(&s->caps,&s->hid):cm_udp_open(&s->caps,&s->hid);
    if(e){destroy(s);return neterr(e);}return publish(p,s,(unsigned)type);
}
static void encode(cm_linux_sockaddr_in_t *a,uint32_t ip,uint16_t port)
{
    memset(a,0,sizeof(*a));a->family=2;a->port[0]=(uint8_t)(port>>8);a->port[1]=(uint8_t)port;
    for(unsigned i=0;i<4;i++)a->address[i]=(uint8_t)(ip>>(24-8*i));
}
static int decode(cm_proc_t *p,uint64_t address,uint64_t length,uint32_t *ip,uint16_t *port)
{
    if(length<16 || length>128)return -22;
    cm_linux_sockaddr_in_t a;if(cm_copyin(p->elf,&a,address,sizeof(a)))return -14;
    if(a.family!=2)return -97;
    *port=(uint16_t)((a.port[0]<<8)|a.port[1]);*ip=0;
    for(unsigned i=0;i<4;i++)*ip=(*ip<<8)|a.address[i];
    return 0;
}
/* Wildcard binds select one primary interface (or lo for UDP without NIC).
 * This is explicitly not a multi-interface INADDR_ANY implementation. */
static int interface(uint32_t ip,int local,int kind)
{
    if(local && ip)for(int i=0;i<CM_MAX_IFACE;i++){cm_iface_t *n=cm_if_get(i);if(n && n->up && n->addr==ip)return kind==1 && ip==CM_UDP_LO_ADDR?unsupported():i;}
    if(local && ip)return -99;
    if(!local && (ip>>24)==127){if(kind==1)return unsupported();return cm_if_find("lo");}
    for(unsigned i=0;i<2;i++){int id=cm_if_find(i?"enet0":"vnet0");cm_iface_t *n=cm_if_get(id);if(n && n->up)return id;}
    if(kind==2 && local)return cm_if_find("lo");
    return -101;
}
static int udp_bind(linux_socket_t *s,int id,uint16_t port)
{
    int e=CM_ERR_EXIST;
    for(unsigned i=0;i<(port?1u:64u);i++) {
        uint16_t candidate=port;if(!candidate){candidate=(uint16_t)(49152+(__atomic_fetch_add(&udp_next,1,__ATOMIC_RELAXED)%16384));}
        e=cm_udp_bind(&s->caps,s->hid,id,candidate);
        if(!e){s->bound=1;s->ifid=id;s->sport=candidate;s->local=cm_if_get(id)->addr;return 0;}
        if(e!=CM_ERR_EXIST || port)break;
    }
    return neterr(e);
}
static int bind_to(cm_proc_t *p,uint64_t fd,uint64_t address,uint64_t length)
{
    cm_object_t *o;int e=get(p,fd,CM_RIGHT_WRITE,&o);if(e)return e;
    linux_socket_t *s=o->payload;uint32_t ip;uint16_t port;
    if(s->bound || s->connected || s->listening || s->busy){e=-22;goto out;}
    e=decode(p,address,length,&ip,&port);if(e)goto out;
    int id=interface(ip,1,s->kind);if(id<0){e=id;goto out;}
    if(s->kind==2)e=udp_bind(s,id,port);
    else {e=neterr(cm_tcp_bind(&s->caps,s->hid,id,port));if(!e){cm_tcp_endpoints_t a;(void)cm_tcp_endpoints(&s->caps,s->hid,&a);s->bound=1;s->ifid=id;s->local=a.local;s->sport=a.sport;}}
out:cm_obj_release(o);return e;
}
static int connect_to(cm_proc_t *p,uint64_t fd,uint64_t address,uint64_t length)
{
    cm_object_t *o;int e=get(p,fd,CM_RIGHT_WRITE,&o);if(e)return e;
    linux_socket_t *s=o->payload;uint32_t ip;uint16_t port;
    e=decode(p,address,length,&ip,&port);if(e)goto out;
    if(!ip || !port){e=-22;goto out;}if(s->busy){e=-114;goto out;}
    if(s->kind==1 && (s->connected || s->listening)){e=-106;goto out;}
    int id=interface(ip,0,s->kind);if(id<0){e=id;goto out;}
    if(s->bound && s->ifid!=id){e=-99;goto out;}
    if(s->kind==1) {
        if(s->nonblock){e=unsupported();goto out;}
        s->busy=1;int raw=cm_tcp_connect(&s->caps,s->hid,id,ip,port,WAIT_MS);s->busy=0;cm_usermode_resume_entry();
        e=raw==CM_ERR_DEAD?-111:raw==CM_ERR_AGAIN?-110:neterr(raw);
        if(e)goto out;
        cm_tcp_endpoints_t a;(void)cm_tcp_endpoints(&s->caps,s->hid,&a);s->local=a.local;s->sport=a.sport;s->bound=1;
    } else if(!s->bound){e=udp_bind(s,id,0);if(e)goto out;}
    s->ifid=id;s->remote=ip;s->dport=port;s->connected=1;e=0;
out:cm_obj_release(o);return e;
}
static int listen_on(cm_proc_t *p,uint64_t fd,uint64_t backlog)
{
    cm_object_t *o;int e=get(p,fd,CM_RIGHT_WRITE,&o);if(e)return e;linux_socket_t *s=o->payload;
    if(s->kind!=1)e=unsupported();
    else if((int64_t)backlog<0)e=-22;
    else if(s->connected || s->busy)e=-22;
    else {
        if(!s->bound){int id=interface(0,1,1);if(id<0)e=id;else {e=neterr(cm_tcp_bind(&s->caps,s->hid,id,0));if(!e){cm_tcp_endpoints_t a;(void)cm_tcp_endpoints(&s->caps,s->hid,&a);s->ifid=id;s->local=a.local;s->sport=a.sport;s->bound=1;}}}
        if(!e){e=neterr(cm_tcp_listen(&s->caps,s->hid,s->ifid,s->sport,(unsigned)CM_MIN(backlog?backlog:1,CM_TCP_BACKLOG_MAX)));if(!e)s->listening=1;}
    }
    cm_obj_release(o);return e;
}
static int address_check(cm_proc_t *p,uint64_t address,uint64_t length,uint32_t *capacity)
{
    *capacity=0;if(!address)return 0;
    if(!length || cm_copyin(p->elf,capacity,length,4) || !cm_user_access(p->elf,length,4,1) ||
       !cm_user_access(p->elf,address,CM_MIN(*capacity,16u),1))return -14;
    return 0;
}
static int address_out(cm_proc_t *p,uint64_t address,uint64_t length,uint32_t capacity,uint32_t ip,uint16_t port)
{
    if(!address)return 0;
    cm_linux_sockaddr_in_t a;encode(&a,ip,port);uint32_t actual=16;
    if(cm_copyout(p->elf,address,&a,CM_MIN(capacity,16u)) || cm_copyout(p->elf,length,&actual,4))return -14;
    return 0;
}
static int accept_on(cm_proc_t *p,uint64_t fd,uint64_t address,uint64_t length,uint64_t flags)
{
    if(flags&~(NONBLOCK|CLOEXEC))return unsupported();
    cm_object_t *o;int e=get(p,fd,CM_RIGHT_READ|CM_RIGHT_WRITE,&o);if(e)return e;linux_socket_t *s=o->payload;
    uint32_t capacity;e=address_check(p,address,length,&capacity);if(e)goto out;
    if(s->kind!=1 || !s->listening){e=-22;goto out;}if(room(p)<0){e=-24;goto out;}
    uint64_t end=deadline();int64_t h;
    for(;;){h=cm_tcp_accept(&s->caps,s->hid);if(h!=CM_ERR_AGAIN)break;if(s->nonblock || (e=turn(p,end)))break;}
    if(h<0){if(!e)e=neterr((int)h);goto out;}
    linux_socket_t *next=allocate(1);if(!next){(void)cm_tcp_close(&s->caps,(int)h,CLOSE_MS);e=-12;goto out;}
    int raw=cm_handle_duplicate(&s->caps,(int)h,&next->caps,&next->hid,CM_RIGHT_ALL);(void)cm_handle_drop(&s->caps,(int)h);
    if(raw){destroy(next);e=neterr(raw);goto out;}
    cm_tcp_endpoints_t a;(void)cm_tcp_endpoints(&next->caps,next->hid,&a);
    next->bound=next->connected=1;next->ifid=a.ifid;next->local=a.local;next->sport=a.sport;next->remote=a.remote;next->dport=a.dport;next->nonblock=!!(flags&NONBLOCK);
    e=address_out(p,address,length,capacity,a.remote,a.dport);if(e){destroy(next);goto out;}
    e=publish(p,next,(unsigned)flags);
out:cm_usermode_resume_entry();cm_obj_release(o);return e;
}
static int64_t io(cm_proc_t *p,uint64_t fd,uint64_t address,uint64_t n,int write,uint64_t flags,uint64_t peer,uint64_t peerlen)
{
    if(flags&~(MSG_DONTWAIT|(write?MSG_NOSIGNAL:0u)))return unsupported();
    cm_object_t *o;int e=get(p,fd,write?CM_RIGHT_WRITE:CM_RIGHT_READ,&o);if(e)return e;
    linux_socket_t *s=o->payload;int64_t result;uint32_t ip=0,capacity=0;uint16_t port=0;
    if(!cm_user_access(p->elf,address,n,!write)){result=-14;goto out;}
    if(s->kind==2 && !n){result=unsupported();goto out;}
    if(s->kind==1 && (write && peer)){result=unsupported();goto out;}
    if(s->kind==1 && !s->connected){result=-107;goto out;}
    if(!n){result=0;goto out;}
    if(s->kind==2 && write && n>CM_UDP_PAYLOAD){result=-90;goto out;}
    if(s->kind==2 && write) {
        if(peer){e=decode(p,peer,peerlen,&ip,&port);if(e){result=e;goto out;}}
        else if(s->connected){ip=s->remote;port=s->dport;}else {result=-89;goto out;}
        if(!ip || !port){result=-22;goto out;}
        int id=interface(ip,0,2);if(id<0){result=id;goto out;}
        if(!s->bound){e=udp_bind(s,id,0);if(e){result=e;goto out;}}
        if(s->ifid!=id){result=-99;goto out;}
    }
    if(!write && peer){e=address_check(p,peer,peerlen,&capacity);if(e){result=e;goto out;}}
    if(s->kind==2 && !s->bound){result=-22;goto out;}
    if(s->kind==1 && write && (s->nonblock || (flags&MSG_DONTWAIT))){result=unsupported();goto out;}
    uint32_t amount=(uint32_t)CM_MIN(n,s->kind==1?CM_PAGE_SIZE:CM_UDP_PAYLOAD);
    void *buffer=cm_page_alloc(0);if(!buffer){result=-12;goto out;}
    if(write) {
        if(cm_copyin(p->elf,buffer,address,amount)){result=-14;goto free_buffer;}
        if(s->kind==1){int raw=(int)cm_tcp_send(&s->caps,s->hid,buffer,amount,WAIT_MS);result=raw==CM_ERR_BUSY?-11:neterr(raw);}
        else {uint64_t end=deadline();int raw;
            do{raw=cm_udp_sendaddr(&s->caps,s->hid,ip,port,buffer,(uint16_t)amount);
                if(raw!=CM_ERR_AGAIN || s->nonblock || (flags&MSG_DONTWAIT))break;
                e=turn(p,end);if(e)break;
            }while(1);if(e)result=e;else if(raw)result=neterr(raw);else result=amount;}
    } else {
        uint64_t end=deadline();unsigned discarded=0;
        for(;;) {
            if(s->kind==1){result=cm_tcp_recv(&s->caps,s->hid,buffer,amount);ip=s->remote;port=s->dport;}
            else {cm_udp_datagram_t *d=buffer;int raw=cm_udp_recv(&s->caps,s->hid,d);result=raw;
                if(!raw){if(s->connected && (d->saddr!=s->remote || d->sport!=s->dport)){
                    if(s->nonblock || (flags&MSG_DONTWAIT)){if(++discarded>=CM_UDP_QUEUE){result=-11;goto free_buffer;}continue;}
                    if((e=turn(p,end))){result=e;goto free_buffer;}continue;}
                    result=CM_MIN(amount,d->len);ip=d->saddr;port=d->sport;for(int64_t i=0;i<result;i++)((uint8_t *)buffer)[i]=d->data[i];}}
            if(result!=CM_ERR_AGAIN)break;
            if(s->nonblock || (flags&MSG_DONTWAIT))break;
            e=turn(p,end);if(e){result=e;goto free_buffer;}
        }
        if(result<0)result=neterr((int)result);
        else if(result>0 && cm_copyout(p->elf,address,buffer,(uint64_t)result))result=-14;
        if(result>=0 && peer && address_out(p,peer,peerlen,capacity,ip,port))result=-14;
    }
free_buffer:cm_page_free(buffer,0);
out:cm_usermode_resume_entry();cm_obj_release(o);return result;
}
int64_t cm_linux_socket_io(cm_proc_t *p,uint64_t fd,uint64_t address,uint64_t n,int write)
{return io(p,fd,address,n,write,0,0,0);}
int64_t cm_linux_socket_dup(cm_proc_t *p,uint64_t old,uint64_t target)
{
    cm_object_t *o;int e=get(p,old,CM_RIGHT_DUPLICATE,&o);if(e)return e;cm_obj_release(o);
    if(target>=CM_FD_MAX)return -9;
    if(old==target)return (int64_t)target;
    int hid;e=cm_handle_duplicate(p->task,p->fd[old],p->task,&hid,CM_RIGHT_ALL);if(e)return neterr(e);
    if(p->fd[target]>=0){int64_t ignored;(void)cm_linux_file_call(3,target,0,0,0,&ignored);}
    p->fd[target]=hid;p->linux_console[target]=0;p->linux_cloexec[target]=0;if(target<3)p->stdio_mask|=1u<<target;
    return (int64_t)target;
}
int64_t cm_linux_socket_fcntl(cm_proc_t *p,uint64_t fd,uint64_t op,uint64_t arg)
{
    cm_object_t *o;int e=get(p,fd,0,&o);if(e)return e;linux_socket_t *s=o->payload;
    if(op==3)e=2|(s->nonblock?NONBLOCK:0);
    else if(op==4){if(arg&~(3u|NONBLOCK))e=unsupported();else {s->nonblock=!!(arg&NONBLOCK);e=0;}}
    else e=unsupported();
    cm_obj_release(o);return e;
}
int cm_linux_socket_call(uint32_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d,int64_t *out)
{
    cm_proc_t *p=cm_proc_current();
    switch(nr) {
    case 41:*out=socket_new(p,a,b,c);break;
    case 42:*out=connect_to(p,a,b,c);break;
    case 43:*out=accept_on(p,a,b,c,0);break;
    case 288:*out=accept_on(p,a,b,c,d);break;
    case 49:*out=bind_to(p,a,b,c);break;
    case 50:*out=listen_on(p,a,b);break;
    case 44:case 45:*out=io(p,a,b,c,nr==44,d,p->linux_regs.r8,p->linux_regs.r9);break;
    case 51:case 52: {
        cm_object_t *o;int e=get(p,a,CM_RIGHT_READ,&o);if(e){*out=e;break;}linux_socket_t *s=o->payload;
        uint32_t capacity;e=address_check(p,b,c,&capacity);
        if(!e && !b)e=-14;
        if(!e && nr==52 && !s->connected)e=-107;
        if(!e)e=address_out(p,b,c,capacity,nr==52?s->remote:s->local,nr==52?s->dport:s->sport);
        cm_obj_release(o);*out=e;break;
    }
    default:return 0; /* setsockopt/getsockopt/shutdown/... -> common ENOSYS counter */
    }
    return 1;
}

int cm_linux_socket_kind(cm_proc_t *p,uint64_t fd)
{
    cm_object_t *o;if(get(p,fd,0,&o))return 0;
    int kind=((linux_socket_t *)o->payload)->kind;cm_obj_release(o);return kind;
}
