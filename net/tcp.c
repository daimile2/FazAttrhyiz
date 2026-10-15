#include "capmix/tcp.h"
#include "capmix/boot.h"
#include "capmix/arch.h"
#include "capmix/sched.h"
#include "capmix/cap_internal.h"
#include "capmix/klog.h"
#include <string.h>
#define FIN 1u
#define SYN 2u
#define RST 4u
#define PSH 8u
#define ACK 16u
enum {CLOSED,SYN_SENT,ESTABLISHED,FIN_WAIT_1,FIN_WAIT_2,CLOSING,TIME_WAIT,CLOSE_WAIT,LAST_ACK,ERROR};
struct pending {
    unsigned active,sent,retries,rto_ms,length;
    uint32_t seq;
    uint8_t flags,data[CM_TCP_MSS];
    uint64_t next,end;
};
struct connection {
    unsigned used,state,busy,ack_pending,peer_fin;
    int ifid;
    uint32_t local,remote,snd_una,snd_nxt,rcv_nxt;
    uint16_t sport,dport,peer_window,mss;
    unsigned head,count;
    uint8_t rx[CM_TCP_RX_BYTES];
    struct pending pending;
    unsigned tx_busy,drop_mask;
    uint64_t generation;
};
struct wire {int ifid;uint32_t local,remote,rcv_nxt;uint16_t sport,dport,window;};
struct segment {
    uint32_t src,dst,seq,ack;
    uint16_t sport,dport,window,mss,length;
    uint8_t flags;
    const uint8_t *data;
};
static struct connection connections[CM_TCP_CONNECTIONS];
static unsigned tcp_lock;
static uint16_t next_port=49152;
static uint64_t retransmits;
static uint64_t lock(void)
{uint64_t f=cm_irq_save();while(__atomic_exchange_n(&tcp_lock,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return f;}
static void unlock(uint64_t f){__atomic_store_n(&tcp_lock,0,__ATOMIC_RELEASE);cm_irq_restore(f);}
static int before(uint32_t a,uint32_t b){return (int32_t)(a-b)<0;}
static uint64_t interval(unsigned ms){return (cm_tsc_hz()/1000)*ms;}
static void clear_pending(struct connection *c)
{memset(&c->pending,0,sizeof(c->pending));c->generation++;}
static void retain(struct connection *c,uint8_t flags,uint32_t seq,const void *data,unsigned n,uint64_t now,uint64_t end)
{
    clear_pending(c);struct pending *p=&c->pending;
    p->active=1;p->flags=flags;p->seq=seq;p->length=n;p->rto_ms=CM_TCP_RTO_MS;p->next=now;p->end=end;
    if(n)memcpy(p->data,data,n);
}
static void acknowledge(struct connection *c,uint64_t now,uint64_t ticks_per_ms)
{
    struct pending *p=&c->pending;
    if(!p->active || !before(p->seq,c->snd_una))return;
    unsigned acked=c->snd_una-p->seq,space=p->length+!!(p->flags&SYN)+!!(p->flags&FIN);
    if(acked>=space){clear_pending(c);return;}
    /* stop-and-wait data can be partially acknowledged. Keep only the tail. */
    for(unsigned i=0;i<p->length-acked;i++)p->data[i]=p->data[i+acked];
    p->length-=acked;p->seq+=acked;p->rto_ms=CM_TCP_RTO_MS;p->retries=0;
    p->next=now+ticks_per_ms*p->rto_ms;c->generation++;
}
/* No wall-clock wait: deterministic tests pass synthetic now values. */
static int due(const struct connection *c,uint64_t now)
{
    const struct pending *p=&c->pending;
    if(!p->active || c->tx_busy || c->state==ERROR || c->state==CLOSED)return 0;
    if(now>=p->end)return -1;
    if(now<p->next)return 0;
    if(p->sent && p->retries>=CM_TCP_RETRIES)return -1;
    return p->sent?2:1;
}
static void submitted(struct pending *p,uint64_t now,int retry,uint64_t ticks_per_ms)
{
    if(retry){p->retries++;p->rto_ms=CM_MIN(p->rto_ms*2,CM_TCP_RTO_MAX_MS);}
    p->sent=1;p->next=now+ticks_per_ms*p->rto_ms;
}
static uint16_t rd16(const uint8_t *p){return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t rd32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void wr16(uint8_t *p,uint16_t x){p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void wr32(uint8_t *p,uint32_t x){p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static uint32_t sum(uint32_t s,const uint8_t *p,unsigned n)
{while(n>=2){s+=rd16(p);p+=2;n-=2;}if(n)s+=(uint32_t)*p<<8;return s;}
static uint16_t finish(uint32_t s){while(s>>16)s=(s&65535)+(s>>16);return (uint16_t)~s;}
static uint16_t checksum(const uint8_t *ip,const uint8_t *tcp,unsigned n)
{return finish(sum(sum(0,ip+12,8)+CM_IPPROTO_TCP+n,tcp,n));}
static unsigned encode(uint8_t *p,const struct wire *c,uint8_t flags,uint32_t seq,const void *data,unsigned n)
{
    unsigned header=(flags&SYN)?24:20,total=14+20+header+n;memset(p,0,total);
    wr16(p+12,CM_ETH_IP);uint8_t *ip=p+14,*t=ip+20;
    ip[0]=0x45;ip[8]=64;ip[9]=CM_IPPROTO_TCP;wr16(ip+2,(uint16_t)(total-14));wr16(ip+6,0x4000);
    wr32(ip+12,c->local);wr32(ip+16,c->remote);wr16(ip+10,finish(sum(0,ip,20)));
    wr16(t,c->sport);wr16(t+2,c->dport);wr32(t+4,seq);wr32(t+8,(flags&ACK)?c->rcv_nxt:0);
    t[12]=(uint8_t)((header/4)<<4);t[13]=flags;wr16(t+14,c->window);
    if(flags&SYN){t[20]=2;t[21]=4;wr16(t+22,CM_TCP_MSS);}
    if(n)memcpy(t+header,data,n);
    wr16(t+16,checksum(ip,t,header+n));return total;
}
static cm_error_t decode(const uint8_t *p,unsigned n,struct segment *out)
{
    if(!p || n<54 || n>CM_PKT_MAX || rd16(p+12)!=CM_ETH_IP)return CM_ERR_INVAL;
    const uint8_t *ip=p+14,*t=ip+20;unsigned total=rd16(ip+2);
    if(ip[0]!=0x45 || !ip[8] || ip[9]!=CM_IPPROTO_TCP || total<40 || total>n-14 ||
       (rd16(ip+6)&~0x4000u) || finish(sum(0,ip,20)))return CM_ERR_INVAL;
    unsigned h=(t[12]>>4)*4;
    if(h<20 || h>60 || h>total-20 || (t[12]&15) || (t[13]&~(FIN|SYN|RST|PSH|ACK)) ||
       !rd16(t) || !rd16(t+2) || checksum(ip,t,total-20))return CM_ERR_INVAL;
    struct segment s={.src=rd32(ip+12),.dst=rd32(ip+16),.seq=rd32(t+4),.ack=rd32(t+8),
        .sport=rd16(t),.dport=rd16(t+2),.window=rd16(t+14),.mss=CM_TCP_MSS,
        .length=(uint16_t)(total-20-h),.flags=t[13],.data=t+h};
    for(unsigned o=20;o<h;) {
        unsigned option=t[o++];if(!option)break;if(option==1)continue;
        if(o==h)return CM_ERR_INVAL;
        unsigned length=t[o++];if(length<2 || length-2>h-o)return CM_ERR_INVAL;
        if(option==2){if(length!=4 || !rd16(t+o))return CM_ERR_INVAL;s.mss=CM_MIN(CM_TCP_MSS,rd16(t+o));}
        o+=length-2;
    }
    *out=s;return CM_OK;
}
/* Pure locked state transition: never sends, waits, logs or touches caps. */
static cm_error_t advance(struct connection *c,const struct segment *s)
{
    if(c->state==SYN_SENT) {
        if((s->flags&(RST|ACK))==(RST|ACK) && s->ack==c->snd_nxt){c->state=ERROR;c->ack_pending=0;clear_pending(c);return CM_ERR_DEAD;}
        if((s->flags&(SYN|ACK|FIN|RST))!=(SYN|ACK) || s->ack!=c->snd_nxt || s->length)return CM_ERR_INVAL;
        c->snd_una=s->ack;acknowledge(c,cm_rdtsc(),cm_tsc_hz()/1000);c->rcv_nxt=s->seq+1;c->mss=s->mss;c->peer_window=s->window;
        c->state=ESTABLISHED;c->ack_pending=1;return CM_OK;
    }
    if(c->state==CLOSED || c->state==ERROR)return CM_ERR_DEAD;
    if(s->flags&RST) {
        if(s->seq!=c->rcv_nxt){c->ack_pending=1;return CM_ERR_INVAL;}
        c->state=ERROR;c->ack_pending=0;clear_pending(c);return CM_ERR_DEAD;
    }
    if(!(s->flags&ACK) || (s->flags&SYN) || before(c->snd_nxt,s->ack)) {c->ack_pending=1;return CM_ERR_INVAL;}
    if(before(c->rcv_nxt,s->seq)){c->ack_pending=1;return CM_ERR_AGAIN;} /* no reorder queue */
    if(c->rcv_nxt-s->seq > (unsigned)s->length+((s->flags&FIN)?1u:0u)) {
        c->ack_pending=1;return CM_ERR_AGAIN; /* wholly stale segment: no ACK/window update */
    }
    if(!before(s->ack,c->snd_una)) {
        c->snd_una=s->ack;acknowledge(c,cm_rdtsc(),cm_tsc_hz()/1000);
        if(s->seq==c->rcv_nxt)c->peer_window=s->window;
    }
    if(c->snd_una==c->snd_nxt) {
        if(c->state==FIN_WAIT_1)c->state=FIN_WAIT_2;
        else if(c->state==CLOSING)c->state=TIME_WAIT;
        else if(c->state==LAST_ACK)c->state=CLOSED;
    }
    unsigned skip=c->rcv_nxt-s->seq,n=s->length;
    if(skip>n){if(n || (s->flags&FIN))c->ack_pending=1;return CM_OK;}
    n-=skip;
    if(n && (c->peer_fin || n>CM_TCP_RX_BYTES-c->count)){c->ack_pending=1;return CM_ERR_AGAIN;}
    for(unsigned i=0;i<n;i++)c->rx[(c->head+c->count+i)%CM_TCP_RX_BYTES]=s->data[skip+i];
    c->count+=n;c->rcv_nxt+=n;
    if(n)c->ack_pending=1;
    if((s->flags&FIN) && !c->peer_fin && s->seq+s->length==c->rcv_nxt) {
        c->rcv_nxt++;c->peer_fin=1;c->ack_pending=1;
        if(c->state==ESTABLISHED)c->state=CLOSE_WAIT;
        else if(c->state==FIN_WAIT_1)c->state=CLOSING;
        else if(c->state==FIN_WAIT_2)c->state=TIME_WAIT;
    }
    return CM_OK;
}
static cm_error_t emit(struct connection *c,uint8_t flags,uint32_t seq,const void *data,unsigned length)
{
    /* Snapshot only metadata, never copy a 4KiB RX buffer onto the kernel stack. */
    struct wire meta={0};uint64_t f=lock();
    if((c->state==ERROR || c->state==CLOSED) && !(flags&RST)){unlock(f);return CM_ERR_DEAD;}
    meta.ifid=c->ifid;meta.local=c->local;meta.remote=c->remote;meta.sport=c->sport;
    meta.dport=c->dport;meta.rcv_nxt=c->rcv_nxt;meta.window=(uint16_t)(CM_TCP_RX_BYTES-c->count);unlock(f);
    uint8_t frame[58+CM_TCP_MSS];unsigned n=encode(frame,&meta,flags,seq,data,length);
    return cm_net_wire_send(meta.ifid,meta.remote,frame,(uint16_t)n);
}
static void flush_ack(struct connection *c)
{
    uint64_t f=lock();unsigned pending=c->ack_pending && c->state!=ERROR && c->state!=CLOSED;uint32_t seq=c->snd_nxt;
    c->ack_pending=0;unlock(f);
    if(pending && emit(c,ACK,seq,NULL,0)) {f=lock();c->ack_pending=1;unlock(f);}
}
cm_error_t cm_tcp_input(int id,const uint8_t *packet,uint16_t length)
{
    struct segment s;cm_error_t e=decode(packet,length,&s);if(e)return e;
    cm_iface_t *iface=cm_if_get(id);if(!iface || !iface->up || s.dst!=iface->addr)return CM_ERR_INVAL;
    if(cm_net_rx(id,packet,length)!=CM_XDP_PASS)return CM_ERR_DENIED;
    uint64_t f=lock();struct connection *c=NULL;
    for(unsigned i=0;i<CM_TCP_CONNECTIONS;i++) {
        struct connection *x=&connections[i];
        if(x->used && x->ifid==id && x->local==s.dst && x->remote==s.src && x->sport==s.dport && x->dport==s.sport){c=x;break;}
    }
    if(!c){unlock(f);return CM_ERR_NOENT;}
    unsigned old=c->state;e=advance(c,&s);int established=old==SYN_SENT && c->state==ESTABLISHED;
    /* Keep this slot alive against final cap release while TX runs unlocked. */
    c->used++;unlock(f);
    if(established)kinfo("tcp established: if=%s local=%u peer_port=%u",iface->name,c->sport,c->dport);
    flush_ack(c);
    f=lock();c->used--;if(!c->used)memset(c,0,sizeof(*c));unlock(f);return e;
}
static void abort_connection(struct connection *c);
static void destroy(void *payload)
{
    struct connection *c=payload;abort_connection(c);uint64_t f=lock();
    c->used--;if(!c->used)memset(c,0,sizeof(*c));unlock(f);
}
static cm_error_t acquire(cm_task_t *task,int hid,cm_rights_t rights,cm_object_t **out)
{
    cm_error_t e=cm_handle_acquire(task,hid,CM_OBJ_PORT,rights,out);if(e)return e;
    if((*out)->destroy!=destroy){cm_obj_release(*out);return CM_ERR_INVAL;}return CM_OK;
}
cm_error_t cm_tcp_init(void)
{memset(connections,0,sizeof(connections));next_port=49152;retransmits=0;return CM_OK;}
unsigned cm_tcp_live(void)
{uint64_t f=lock();unsigned n=0;for(unsigned i=0;i<CM_TCP_CONNECTIONS;i++)n+=connections[i].used!=0;unlock(f);return n;}
cm_error_t cm_tcp_open(cm_task_t *task,int *out)
{
    if(!task || !out)return CM_ERR_INVAL;
    uint64_t f=lock();struct connection *c=NULL;
    for(unsigned i=0;i<CM_TCP_CONNECTIONS;i++)if(!connections[i].used){c=&connections[i];memset(c,0,sizeof(*c));c->used=1;break;}
    unlock(f);if(!c)return CM_ERR_NOSPC;
    cm_rights_t rights=CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE;
    cm_object_t *obj=cm_obj_mint_owned(CM_OBJ_PORT,c,rights,destroy);if(!obj){destroy(c);return CM_ERR_NOSPC;}
    int hid=cm_handle_install(task,obj,rights);cm_obj_release(obj);if(hid<0)return hid;*out=hid;return CM_OK;
}
static void turn(void){if(cm_sched_current())cm_sched_yield();else (void)cm_sched_run_until_idle(1);}
static uint64_t deadline(unsigned ms){return cm_rdtsc()+(cm_tsc_hz()/1000)*ms;}
static int timeout_ok(unsigned ms){return ms && ms<=10000;}
static void abort_connection(struct connection *c)
{
    uint64_t f=lock();uint32_t seq=c->snd_nxt;unsigned state=c->state;c->state=ERROR;c->busy=0;c->ack_pending=0;clear_pending(c);unlock(f);
    if(state!=CLOSED && state!=ERROR)(void)emit(c,RST|ACK,seq,NULL,0);
}
static void service(struct connection *c,uint64_t now)
{
    uint8_t data[CM_TCP_MSS];uint64_t f=lock();int action=due(c,now);
    if(!action){unlock(f);return;}
    if(action<0) {
        unsigned retries=c->pending.retries;uint32_t seq=c->snd_nxt;
        c->state=ERROR;c->ack_pending=0;c->busy=0;clear_pending(c);unlock(f);
        kinfo("tcp rto exhausted: retries=%u/4 or API deadline; pending cleared error=DEAD RST=attempted",retries);
        (void)emit(c,RST|ACK,seq,NULL,0);return;
    }
    struct pending *p=&c->pending;
    unsigned length=p->length,flags=p->flags,retries=p->retries,rto=p->rto_ms;
    uint32_t seq=p->seq;uint64_t gen=c->generation;unsigned sent=p->sent;
    if(length)memcpy(data,p->data,length);
    unsigned drop_bit=(flags&SYN)?1u:(flags&FIN)?4u:2u;
    int dropped=(c->drop_mask&drop_bit)!=0;if(dropped)c->drop_mask&=~drop_bit;
    c->tx_busy=1;submitted(p,now,action==2,cm_tsc_hz()/1000);unlock(f);
    cm_error_t e=dropped?CM_OK:emit(c,(uint8_t)flags,seq,data,length);
    f=lock();c->tx_busy=0;
    if(action==2 && !e)__atomic_fetch_add(&retransmits,1,__ATOMIC_RELAXED);
    if(c->generation==gen && c->pending.active && e) {
        if(e==CM_ERR_AGAIN) {
            /* ARP/TX backpressure is not a successfully transmitted retry. */
            p->sent=sent;p->retries=retries;p->rto_ms=rto;p->next=now+interval(10);
        } else {c->state=ERROR;c->busy=0;c->ack_pending=0;clear_pending(c);}
    }
    unsigned attempt=action==2?retries+1:0;unlock(f);
    if(dropped)kinfo("tcp test dropped: flags=0x%x seq=%u",flags,seq);
    if(action==2 && !e)kinfo("tcp rto retransmit: flags=0x%x seq=%u bytes=%u retry=%u next_rto_ms=%u",flags,seq,length,attempt,CM_MIN(rto*2,CM_TCP_RTO_MAX_MS));
}
void cm_tcp_poll(void)
{
    for(unsigned i=0;i<CM_TCP_CONNECTIONS;i++) {
        struct connection *c=&connections[i];uint64_t f=lock();
        if(!c->used || !c->pending.active){unlock(f);continue;}
        c->used++;unlock(f);service(c,cm_rdtsc());
        f=lock();c->used--;if(!c->used)memset(c,0,sizeof(*c));unlock(f);
    }
}
uint64_t cm_tcp_retransmits(void){return __atomic_load_n(&retransmits,__ATOMIC_RELAXED);}
cm_error_t cm_tcp_test_drop(cm_task_t *task,int hid,unsigned mask)
{
    if(!cm_boot_is_selftest() || mask&~7u)return CM_ERR_DENIED;
    cm_object_t *o;cm_error_t e=acquire(task,hid,CM_RIGHT_WRITE,&o);if(e)return e;
    struct connection *c=o->payload;uint64_t f=lock();
    if(c->state!=CLOSED || c->busy)e=CM_ERR_BUSY;else c->drop_mask=mask;
    unlock(f);cm_obj_release(o);return e;
}
static cm_error_t send_reserved(struct connection *c,uint8_t flags,uint32_t seq,const void *data,unsigned n,uint64_t end)
{
    uint64_t f=lock();
    if(c->state==ERROR){unlock(f);return CM_ERR_DEAD;}
    retain(c,flags,seq,data,n,cm_rdtsc(),end);unlock(f);
    service(c,cm_rdtsc());return CM_OK;
}
cm_error_t cm_tcp_connect(cm_task_t *task,int hid,int id,uint32_t remote,uint16_t port,unsigned ms)
{
    cm_iface_t *iface=cm_if_get(id);
    if(!iface || !iface->up || iface->addr==CM_UDP_LO_ADDR || !remote || !port || !timeout_ok(ms))return CM_ERR_INVAL;
    cm_object_t *obj;cm_error_t e=acquire(task,hid,CM_RIGHT_WRITE,&obj);if(e)return e;
    struct connection *c=obj->payload;uint64_t f=lock();
    if(c->state!=CLOSED || c->busy){unlock(f);cm_obj_release(obj);return CM_ERR_BUSY;}
    c->ifid=id;c->local=iface->addr;c->remote=remote;c->dport=port;
    int available=0;
    for(unsigned attempt=0;attempt<16384 && !available;attempt++) {
        if(next_port<49152)next_port=49152;
        c->sport=next_port++;available=1;
        for(unsigned i=0;i<CM_TCP_CONNECTIONS;i++)if(&connections[i]!=c && connections[i].used && connections[i].ifid==id && connections[i].sport==c->sport)available=0;
    }
    if(!available){unlock(f);cm_obj_release(obj);return CM_ERR_NOSPC;}
    c->snd_una=(uint32_t)cm_rdtsc();c->snd_nxt=c->snd_una+1;c->state=SYN_SENT;c->mss=CM_TCP_MSS;c->busy=1;
    uint32_t seq=c->snd_una;unlock(f);
    kinfo("tcp syn: if=%s source=%u dest_port=%u timeout_ms=%u",iface->name,c->sport,port,ms);
    uint64_t end=deadline(ms);e=send_reserved(c,SYN,seq,NULL,0,end);
    while(!e && cm_rdtsc()<end) {
        service(c,cm_rdtsc());
        f=lock();unsigned state=c->state;unlock(f);
        if(state==ESTABLISHED){flush_ack(c);break;}if(state==ERROR){e=CM_ERR_DEAD;break;}turn();
    }
    f=lock();if(!e && c->state!=ESTABLISHED)e=CM_ERR_AGAIN;unlock(f);
    if(!e){f=lock();c->busy=0;unlock(f);}
    if(e){abort_connection(c);kinfo("tcp connect failed: error=%d cleanup=abort UDP/DHCP unaffected",e);}
    cm_obj_release(obj);return e;
}
int64_t cm_tcp_send(cm_task_t *task,int hid,const void *data,uint32_t length,unsigned ms)
{
    if(!data || !length || length>65536 || !timeout_ok(ms))return CM_ERR_INVAL;
    cm_object_t *obj;cm_error_t e=acquire(task,hid,CM_RIGHT_WRITE,&obj);if(e)return e;
    struct connection *c=obj->payload;uint64_t f=lock();
    if(c->busy || (c->state!=ESTABLISHED && c->state!=CLOSE_WAIT)){
        cm_error_t status=c->busy?CM_ERR_BUSY:c->state==ERROR?CM_ERR_DEAD:CM_ERR_INVAL;
        unlock(f);cm_obj_release(obj);return status;
    }
    c->busy=1;unlock(f);uint32_t done=0;uint64_t end=deadline(ms);
    while(done<length && cm_rdtsc()<end) {
        f=lock();
        if(c->state==ERROR){unlock(f);e=CM_ERR_DEAD;break;}
        unsigned n=CM_MIN(length-done,CM_MIN(c->mss,c->peer_window));
        if(!n || c->snd_una!=c->snd_nxt){unlock(f);flush_ack(c);turn();continue;}
        uint32_t seq=c->snd_nxt;c->snd_nxt+=n;uint32_t target=c->snd_nxt;unlock(f);
        e=send_reserved(c,ACK|PSH,seq,(const uint8_t *)data+done,n,end);if(e)break;
        for(;;) {
            service(c,cm_rdtsc());
            f=lock();int acked=c->snd_una==target,dead=c->state==ERROR;unlock(f);
            if(acked){done+=n;break;}if(dead){e=CM_ERR_DEAD;break;}
            if(cm_rdtsc()>=end){e=CM_ERR_AGAIN;break;}flush_ack(c);turn();
        }
        if(e)break;
    }
    if(done<length && !e)e=CM_ERR_AGAIN;
    if(e)abort_connection(c);
    else {f=lock();c->busy=0;unlock(f);}
    cm_obj_release(obj);return done?(int64_t)done:e;
}
int64_t cm_tcp_recv(cm_task_t *task,int hid,void *data,uint32_t capacity)
{
    if(!data || !capacity)return CM_ERR_INVAL;
    cm_object_t *obj;cm_error_t e=acquire(task,hid,CM_RIGHT_READ,&obj);if(e)return e;
    struct connection *c=obj->payload;uint64_t f=lock();unsigned n=CM_MIN(capacity,c->count);
    for(unsigned i=0;i<n;i++)((uint8_t *)data)[i]=c->rx[(c->head+i)%CM_TCP_RX_BYTES];
    c->head=(c->head+n)%CM_TCP_RX_BYTES;c->count-=n;if(n)c->ack_pending=1;
    int64_t result=n?(int64_t)n:c->peer_fin?0:c->state==ERROR?CM_ERR_DEAD:CM_ERR_AGAIN;
    unlock(f);flush_ack(c);cm_obj_release(obj);return result;
}
cm_error_t cm_tcp_close(cm_task_t *task,int hid,unsigned ms)
{
    if(!timeout_ok(ms))return CM_ERR_INVAL;
    cm_object_t *obj;cm_error_t e=acquire(task,hid,CM_RIGHT_WRITE,&obj);if(e)return e;
    struct connection *c=obj->payload;uint64_t f=lock();unsigned state=c->state;
    if(c->busy){unlock(f);cm_obj_release(obj);return CM_ERR_BUSY;}
    c->busy=1;uint32_t seq=c->snd_nxt;
    if(state==ESTABLISHED || state==CLOSE_WAIT){c->snd_nxt++;c->state=state==ESTABLISHED?FIN_WAIT_1:LAST_ACK;}
    unlock(f);uint64_t end=deadline(ms);
    if(state==ESTABLISHED || state==CLOSE_WAIT)e=send_reserved(c,FIN|ACK,seq,NULL,0,end);
    while(!e && state!=CLOSED && state!=ERROR && cm_rdtsc()<end) {
        service(c,cm_rdtsc());
        f=lock();state=c->state;unlock(f);
        if(state==CLOSED || state==TIME_WAIT)break;
        if(state==ERROR){e=CM_ERR_DEAD;break;}flush_ack(c);turn();
    }
    f=lock();state=c->state;unlock(f);
    if(!e && state!=CLOSED && state!=ERROR && state!=TIME_WAIT)e=CM_ERR_AGAIN;
    if(state==TIME_WAIT && !e){uint64_t linger=deadline(100);while(cm_rdtsc()<linger)turn();}
    if(e || state==ERROR)abort_connection(c);
    else {f=lock();c->state=CLOSED;c->busy=0;clear_pending(c);unlock(f);}
    kinfo("tcp close: state=%u fin=%s error=%d TIME_WAIT=%ums handle released",state,
          state==TIME_WAIT || state==CLOSED?"complete":"aborted",e,state==TIME_WAIT && !e?100u:0u);
    cm_error_t drop=cm_handle_drop(task,hid);cm_obj_release(obj);return e?e:drop;
}
void cm_tcp_process_exit(cm_task_t *task)
{
    if(!task)return;
    for(int hid=1;hid<CM_HANDLES_PER_TASK;hid++) {
        uint64_t f=cm_cap_lock();cm_handle_t *h=cm_handle_lookup(task,hid);cm_object_t *obj=NULL;
        if(h && h->obj->type==CM_OBJ_PORT && h->obj->destroy==destroy){obj=h->obj;memset(h,0,sizeof(*h));}
        cm_cap_unlock(f);if(obj){abort_connection(obj->payload);cm_obj_release(obj);}
    }
}
static int exhausted_lifecycle_selftest(void)
{
    unsigned before=cm_tcp_live();cm_task_t *task=cm_task_create("tcp-rto-exhaust",CM_QOS_USER_INTERACTIVE);
    if(!task)return 1;
    int hid=-1,alias=-1,failed=0;cm_object_t *obj=NULL;
    if(cm_tcp_open(task,&hid) || acquire(task,hid,CM_RIGHT_WRITE,&obj)){failed=1;goto finish;}
    struct connection *c=obj->payload;uint64_t f=lock();
    c->state=SYN_SENT;c->ifid=-1;c->snd_una=100;c->snd_nxt=101;
    retain(c,SYN,100,NULL,0,100,10000);c->pending.sent=1;c->pending.retries=CM_TCP_RETRIES;c->pending.next=200;
    unlock(f);
    if(cm_handle_duplicate(task,hid,task,&alias,CM_RIGHT_READ)){failed=1;goto finish;}
    /* Real expiration/abort path with a virtual due time; no NIC is needed.
     * RST is attempted on an invalid route and safely fails, never fabricated. */
    service(c,200);
    f=lock();int cleaned=c->state==ERROR && !c->pending.active && !c->pending.next && !c->busy;unlock(f);
    if(!cleaned || cm_tcp_send(task,hid,"x",1,100)!=CM_ERR_DEAD ||
       cm_tcp_recv(task,alias,&failed,1)!=CM_ERR_DEAD)failed=1;
finish:
    if(obj)cm_obj_release(obj);
    if(hid>=0 && cm_tcp_close(task,hid,100)!=CM_OK)failed=1;
    if(alias>=0 && cm_handle_drop(task,alias)!=CM_OK)failed=1;
    cm_tcp_process_exit(task);
    if(cm_tcp_live()!=before)failed=1;
    cm_obj_release(task->self_obj);memset(task,0,sizeof(*task));
    if(!failed)kinfo("tcp rto exhausted cleanup verified: ERROR/RST-attempt pending=0 aliases released live=%u",before);
    return failed;
}
static int retransmission_selftest(void)
{
    static struct connection c;
    const uint8_t body[]={11,22,33,44,55};
    uint64_t step=CM_TCP_RTO_MS,now=123; /* synthetic ticks: 1 per ms */
    if(!step)return 1;
    for(unsigned kind=0;kind<3;kind++) {
        memset(&c,0,sizeof(c));c.state=kind==0?SYN_SENT:kind==1?ESTABLISHED:FIN_WAIT_1;
        unsigned flags=kind==0?SYN:kind==1?ACK|PSH:FIN|ACK;
        retain(&c,(uint8_t)flags,100,body,kind==1?sizeof(body):0,now,now+10000);
        if(due(&c,now)!=1)return 1;
        submitted(&c.pending,now,0,1);
        if(due(&c,c.pending.next-1)!=0 || due(&c,c.pending.next)!=2)return 1;
        for(unsigned retry=1;retry<=CM_TCP_RETRIES;retry++) {
            uint64_t at=c.pending.next;submitted(&c.pending,at,1,1);
            unsigned expected=CM_MIN(CM_TCP_RTO_MS<<retry,CM_TCP_RTO_MAX_MS);
            if(c.pending.retries!=retry || c.pending.rto_ms!=expected ||
               c.pending.next!=at+expected || c.pending.seq!=100 ||
               c.pending.flags!=flags || (kind==1 && memcmp(c.pending.data,body,sizeof(body))))return 1;
        }
        if(due(&c,c.pending.next)!=-1)return 1;
        clear_pending(&c);if(c.pending.active || c.pending.length || c.pending.next)return 1;
    }
    memset(&c,0,sizeof(c));c.state=ESTABLISHED;
    retain(&c,ACK|PSH,UINT32_MAX-1,body,sizeof(body),now,now+10000);
    submitted(&c.pending,now,0,1);c.snd_una=UINT32_MAX-1;
    acknowledge(&c,now+1,1);if(c.pending.seq!=UINT32_MAX-1 || c.pending.length!=5)return 1; /* duplicate ACK */
    c.rcv_nxt=700;c.snd_nxt=3;
    struct segment partial={.flags=ACK,.seq=700,.ack=0,.window=4096};
    if(advance(&c,&partial))return 1;
    if(c.pending.seq || c.pending.length!=3 || memcmp(c.pending.data,body+2,3) || c.pending.retries)return 1;
    partial.ack=3;if(advance(&c,&partial) || c.pending.active)return 1;
    c.state=SYN_SENT;retain(&c,SYN,99,NULL,0,now,now+step);
    c.tx_busy=1;if(due(&c,now)!=0)return 1;c.tx_busy=0;
    if(due(&c,now+step)!=-1)return 1; /* caller deadline precedes retry budget */
    c.snd_nxt=100;c.snd_una=99;
    struct segment reset={.flags=RST|ACK,.ack=100};
    if(advance(&c,&reset)!=CM_ERR_DEAD || c.pending.active || c.state!=ERROR)return 1;
    if(exhausted_lifecycle_selftest())return 1;
    kinfo("tcp rto mechanism verified: SYN/data/FIN retained; 200/400/800/1600ms backoff max-retries=4; partial/duplicate/wrap ACK; deadline/RST cleanup");
    return 0;
}
int cm_tcp_mechanism_selftest(void)
{
    if(retransmission_selftest())return 1;
    uint8_t frame[58+CM_TCP_MSS];struct segment s;
    struct wire wire={.local=0x0a000202,.remote=0x0a00020f,.sport=9998,.dport=49152,.window=4096,.rcv_nxt=101};
    unsigned n=encode(frame,&wire,SYN|ACK,700,NULL,0);
    if(decode(frame,n,&s) || s.seq!=700 || s.ack!=101 || s.mss!=536)return 1;
    frame[50]^=1;if(decode(frame,n,&s)!=CM_ERR_INVAL)return 1;frame[50]^=1;
    if(decode(frame,53,&s)!=CM_ERR_INVAL)return 1;
    /* Isolated fixture, no NIC TX, connection slots or task mutation. */
    static struct connection fixture;
    memset(&fixture,0,sizeof(fixture));fixture.state=SYN_SENT;fixture.snd_una=100;fixture.snd_nxt=101;
    s.flags=RST|ACK;s.ack=102;
    if(advance(&fixture,&s)!=CM_ERR_INVAL || fixture.state!=SYN_SENT)return 1;
    s.ack=101;if(advance(&fixture,&s)!=CM_ERR_DEAD || fixture.state!=ERROR)return 1;
    fixture.state=SYN_SENT;
    if(decode(frame,n,&s) || advance(&fixture,&s) || fixture.state!=ESTABLISHED || fixture.rcv_nxt!=701)return 1;
    static const uint8_t body[]={1,2,3,4,5};wire.rcv_nxt=101;
    n=encode(frame,&wire,ACK|PSH,701,body,sizeof(body));
    if(decode(frame,n,&s) || advance(&fixture,&s) || fixture.count!=5 || fixture.rcv_nxt!=706)return 1;
    if(advance(&fixture,&s) || fixture.count!=5)return 1; /* duplicate does not enqueue twice */
    s.seq=708;if(advance(&fixture,&s)!=CM_ERR_AGAIN || fixture.count!=5)return 1;
    s.seq=706;s.length=0;s.flags=ACK;s.ack=102;
    if(advance(&fixture,&s)!=CM_ERR_INVAL || fixture.snd_una!=101)return 1;
    s.flags=RST;s.seq=705;
    if(advance(&fixture,&s)!=CM_ERR_INVAL || fixture.state!=ESTABLISHED)return 1;
    s.flags=ACK;s.seq=706;s.ack=101;s.window=0;s.length=0;
    if(advance(&fixture,&s) || fixture.peer_window)return 1;
    fixture.count=CM_TCP_RX_BYTES;s.length=1;
    if(advance(&fixture,&s)!=CM_ERR_AGAIN || fixture.count!=CM_TCP_RX_BYTES || fixture.rcv_nxt!=706)return 1;
    fixture.count=5;s.length=0;s.window=4096;
    s.flags=ACK|FIN;s.seq=706;s.ack=101;
    if(advance(&fixture,&s) || fixture.state!=CLOSE_WAIT || !fixture.peer_fin)return 1;
    fixture.state=LAST_ACK;fixture.snd_nxt=102;s.flags=ACK;s.seq=707;s.ack=102;
    if(advance(&fixture,&s) || fixture.state!=CLOSED)return 1;
    fixture.state=FIN_WAIT_1;fixture.peer_fin=0;fixture.snd_nxt=103;fixture.snd_una=102;
    s.flags=ACK|FIN;s.seq=fixture.rcv_nxt;s.ack=103;
    if(advance(&fixture,&s) || fixture.state!=TIME_WAIT)return 1;
    fixture.state=FIN_WAIT_1;fixture.peer_fin=0;fixture.snd_una=103;fixture.snd_nxt=104;
    s.flags=ACK|FIN;s.seq=fixture.rcv_nxt;s.ack=103;
    if(advance(&fixture,&s) || fixture.state!=CLOSING)return 1;
    s.flags=ACK;s.seq=fixture.rcv_nxt;s.ack=104;
    if(advance(&fixture,&s) || fixture.state!=TIME_WAIT)return 1;
    kinfo("tcp mechanism verified: SYN/ACK data/FIN, checksum/truncated/future-ACK/RST reject, duplicate/reorder");return 0;
}
