#include "capmix/net.h"
#include "capmix/tcp.h"
#include "capmix/virtio_net.h"
#include "capmix/e1000.h"
#include "capmix/arch.h"
#include "capmix/sched.h"
#include "capmix/klog.h"
#include <string.h>
/* One scheduler-owned consumer: INTx notification or timed polling fallback.
 * Driver guards serialize DMA operations; neighbor guard never encloses TX. */
struct neighbor {uint32_t addr;uint8_t mac[6];uint64_t asked;};
static struct neighbor peers[CM_MAX_IFACE];
static volatile unsigned peer_lock;
static cm_thread_t *worker;
static cm_task_t *worker_task;
static unsigned running;
static uint64_t frames,udp_queued[CM_MAX_IFACE];
static unsigned rx_pending,wait_lock;
static uint64_t irq_udp_queued[CM_MAX_IFACE];
_Static_assert(CM_MAX_IFACE<=32,"RX notification bitmap capacity");
static cm_thread_t *sleeper;
static uint64_t wake_at,sleeps,irq_frames,irq_wakes;
static uint64_t wait_guard(void)
{uint64_t f=cm_irq_save();while(__atomic_exchange_n(&wait_lock,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return f;}
static void wait_unlock(uint64_t f)
{__atomic_store_n(&wait_lock,0,__ATOMIC_RELEASE);cm_irq_restore(f);}
void cm_net_rx_notify(int id)
{
    if(id>=0 && id<CM_MAX_IFACE)__atomic_fetch_or(&rx_pending,1u<<id,__ATOMIC_RELEASE);
}
/* Deferred wake, called at scheduler query points outside rq lock, not ISR.
 * The same wait lock serializes prepare/wake/stop across CPUs. */
static void poll_wake(void)
{
    if(!__atomic_load_n(&running,__ATOMIC_ACQUIRE))return;
    if(!__atomic_load_n(&sleeper,__ATOMIC_ACQUIRE))return;
    if(!__atomic_load_n(&rx_pending,__ATOMIC_ACQUIRE) &&
       cm_rdtsc()<__atomic_load_n(&wake_at,__ATOMIC_ACQUIRE))return;
    uint64_t f=wait_guard();unsigned events=__atomic_load_n(&rx_pending,__ATOMIC_ACQUIRE);
    if(sleeper && (events || cm_rdtsc()>=wake_at)) {
        cm_thread_t *th=sleeper;__atomic_store_n(&sleeper,NULL,__ATOMIC_RELEASE);
        if(events)__atomic_fetch_add(&irq_wakes,1,__ATOMIC_RELAXED);
        cm_thread_wake(th);
    }
    wait_unlock(f);
}
static void park_rx(void)
{
    cm_thread_t *self=cm_sched_current();uint64_t f=wait_guard();
    if(__atomic_load_n(&running,__ATOMIC_ACQUIRE) && !__atomic_load_n(&rx_pending,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&wake_at,cm_rdtsc()+cm_tsc_hz()/100,__ATOMIC_RELEASE);
        __atomic_store_n(&sleeper,self,__ATOMIC_RELEASE);__atomic_fetch_add(&sleeps,1,__ATOMIC_RELAXED);
        cm_thread_block_prepare(self,NULL);
        __atomic_store_n(&wait_lock,0,__ATOMIC_RELEASE); /* keep IF=0 until saved */
        cm_arch_ctx_swap(self->ctx_slot,-1);
        cm_irq_restore(f);
    } else {wait_unlock(f);cm_sched_yield();}
}
uint64_t cm_net_bg_irq_frames(void){return __atomic_load_n(&irq_frames,__ATOMIC_RELAXED);}
uint64_t cm_net_bg_sleeps(void){return __atomic_load_n(&sleeps,__ATOMIC_RELAXED);}
int cm_net_bg_is_parked(void)
{uint64_t f=wait_guard();int parked=sleeper!=NULL;wait_unlock(f);return parked;}
uint64_t cm_net_bg_irq_udp_queued(int id)
{return id>=0 && id<CM_MAX_IFACE?__atomic_load_n(&irq_udp_queued[id],__ATOMIC_RELAXED):0;}
static uint64_t guard(void)
{uint64_t f=cm_irq_save();while(__atomic_exchange_n(&peer_lock,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return f;}
static void unguard(uint64_t f){__atomic_store_n(&peer_lock,0,__ATOMIC_RELEASE);cm_irq_restore(f);}
static uint16_t rd16(const uint8_t *p){return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t rd32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void wr16(uint8_t *p,uint16_t x){p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void wr32(uint8_t *p,uint32_t x){p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static cm_error_t tx(int id,const uint8_t *data,uint16_t n)
{
    if(cm_virtio_net_available() && id==cm_virtio_net_ifid())return cm_virtio_net_transmit(data,n);
    if(cm_e1000_available() && id==cm_e1000_ifid())return cm_e1000_transmit(data,n);
    return CM_ERR_NOTSUP;
}
static cm_error_t rx(int id,uint8_t *data,uint16_t *n)
{
    if(cm_virtio_net_available() && id==cm_virtio_net_ifid())return cm_virtio_net_receive(data,n);
    if(cm_e1000_available() && id==cm_e1000_ifid())return cm_e1000_receive(data,n);
    return CM_ERR_NOTSUP;
}
static void arp(uint8_t *frame,cm_iface_t *iface,uint32_t dest,const uint8_t *mac,unsigned op)
{
    memset(frame,0,60);if(mac)memcpy(frame,mac,6);else memset(frame,255,6);
    memcpy(frame+6,iface->mac,6);wr16(frame+12,0x806);
    uint8_t *a=frame+14;wr16(a,1);wr16(a+2,CM_ETH_IP);a[4]=6;a[5]=4;wr16(a+6,(uint16_t)op);
    memcpy(a+8,iface->mac,6);wr32(a+14,iface->addr);
    if(mac)memcpy(a+18,mac,6);
    wr32(a+24,dest);
}
cm_error_t cm_net_wire_send(int id,uint32_t dest,uint8_t *frame,uint16_t bytes)
{
    cm_iface_t *iface=cm_if_get(id);
    if(!iface || !iface->up || !__atomic_load_n(&running,__ATOMIC_ACQUIRE))return CM_ERR_DEAD;
    if(!dest || dest==UINT32_MAX || (dest&0xf0000000u)==0xe0000000u || dest==iface->addr)return CM_ERR_NOTSUP;
    uint32_t hop=(dest&iface->netmask)==(iface->addr&iface->netmask)?dest:iface->gateway;
    if(!hop)return CM_ERR_NOTSUP;
    uint8_t mac[6];int found,request=0;uint64_t f=guard();
    found=peers[id].addr==hop;
    if(found)memcpy(mac,peers[id].mac,6);
    else {
        uint64_t now=cm_rdtsc(),interval=cm_tsc_hz()/10;
        if(!peers[id].asked || now-peers[id].asked>=interval){peers[id].asked=now;request=1;}
    }
    unguard(f);
    if(!found) {
        if(request){uint8_t packet[60];arp(packet,iface,hop,NULL,1);cm_error_t e=tx(id,packet,60);if(e)return e;}
        return CM_ERR_AGAIN; /* caller retries; no UDP silently queued */
    }
    memcpy(frame,mac,6);memcpy(frame+6,iface->mac,6);return tx(id,frame,bytes);
}
static void ingress(int id,uint8_t *packet,uint16_t n,int notified)
{
    cm_iface_t *iface=cm_if_get(id);if(!iface || !iface->up || n<14)return;
    static const uint8_t broadcast[6]={255,255,255,255,255,255};
    if(memcmp(packet,iface->mac,6) && memcmp(packet,broadcast,6))return;
    if(rd16(packet+12)==CM_ETH_IP && n>=34 && packet[23]==CM_IPPROTO_UDP) {
        if(cm_udp_inject(id,packet,n)==CM_OK) {
            __atomic_fetch_add(&udp_queued[id],1,__ATOMIC_RELAXED);
            if(notified)__atomic_fetch_add(&irq_udp_queued[id],1,__ATOMIC_RELAXED);
        }
        return;
    }
    if(rd16(packet+12)==CM_ETH_IP && n>=34 && packet[23]==CM_IPPROTO_TCP){(void)cm_tcp_input(id,packet,n);return;}
    if(cm_net_rx(id,packet,n)!=CM_XDP_PASS || rd16(packet+12)!=0x806 || n<42)return;
    const uint8_t *a=packet+14;unsigned op=rd16(a+6),nonzero=0;
    for(unsigned i=0;i<6;i++)nonzero|=a[8+i];
    if(rd16(a)!=1 || rd16(a+2)!=CM_ETH_IP || a[4]!=6 || a[5]!=4 ||
       (op!=1 && op!=2) || !nonzero || (a[8]&1) || memcmp(packet+6,a+8,6) ||
       !rd32(a+14) || rd32(a+24)!=iface->addr ||
       (rd32(a+14)&iface->netmask)!=(iface->addr&iface->netmask))return;
    if(op==2 && (memcmp(a+18,iface->mac,6) || memcmp(packet,iface->mac,6)))return;
    uint64_t f=guard();peers[id].addr=rd32(a+14);memcpy(peers[id].mac,a+8,6);unguard(f);
    if(op==1){uint8_t reply[60];arp(reply,iface,rd32(a+14),a+8,2);(void)tx(id,reply,60);}
}
static void background(void *arg)
{
    CM_UNUSED(arg);uint8_t packet[CM_PKT_MAX];uint64_t health=0;
    while(__atomic_load_n(&running,__ATOMIC_ACQUIRE)) {
        unsigned events=__atomic_exchange_n(&rx_pending,0,__ATOMIC_ACQ_REL);
        uint64_t now=cm_rdtsc();int watchdog=now>=health;
        if(watchdog)health=now+cm_tsc_hz()/10;
        for(int id=0;id<CM_MAX_IFACE;id++) {
            if(!cm_if_get(id))continue;
            int irq=(cm_e1000_irq_enabled() && id==cm_e1000_ifid()) ||
                    (cm_virtio_net_irq_enabled() && id==cm_virtio_net_ifid());
            if(irq && !(events&(1u<<id)) && !watchdog)continue;
            for(unsigned budget=0;budget<16;budget++) {
                uint16_t bytes=0;cm_error_t e=rx(id,packet,&bytes);
                if(e==CM_ERR_AGAIN || e==CM_ERR_DEAD || e==CM_ERR_NOTSUP)break;
                if(e)continue;
                __atomic_fetch_add(&frames,1,__ATOMIC_RELAXED);
                if(irq && (events&(1u<<id)))__atomic_fetch_add(&irq_frames,1,__ATOMIC_RELAXED);
                ingress(id,packet,bytes,irq && (events&(1u<<id)));
                if(budget==15)__atomic_fetch_or(&rx_pending,1u<<id,__ATOMIC_RELEASE);
            }
        }
        cm_tcp_poll(); /* connection RTO progression, no timer/IRQ changes */
        park_rx();
    }
    cm_sched_current()->state=CM_TH_DEAD;
}
cm_error_t cm_net_bg_start(void)
{
    if(__atomic_load_n(&running,__ATOMIC_ACQUIRE))return CM_OK;
    if(!cm_virtio_net_available() && !cm_e1000_available()){
        kinfo("net bg RX skipped: no supported NIC");return CM_ERR_NOTSUP;
    }
    if(!worker_task)worker_task=cm_task_create("net-rx",CM_QOS_USER_INTERACTIVE);
    if(!worker_task)return CM_ERR_NOSPC;
    memset(peers,0,sizeof(peers));
    cm_sched_set_poll_hook(poll_wake);
    __atomic_store_n(&running,1,__ATOMIC_RELEASE);
    worker=cm_thread_create(worker_task,background,NULL,CM_QOS_USER_INTERACTIVE);
    if(!worker){__atomic_store_n(&running,0,__ATOMIC_RELEASE);return CM_ERR_NOSPC;}
    kinfo("net bg RX started: scheduler thread tid=%llu budget=16/interface IRQ=e1000/virtio-net-if-routed; idle=BLOCKED timer=10ms watchdog=100ms",
          (unsigned long)worker->id);return CM_OK;
}
void cm_net_bg_stop(void)
{
    uint64_t f=wait_guard();__atomic_store_n(&running,0,__ATOMIC_RELEASE);
    if(sleeper){cm_thread_t *th=sleeper;__atomic_store_n(&sleeper,NULL,__ATOMIC_RELEASE);cm_thread_wake(th);}
    wait_unlock(f);
    /* Debug lifecycle only: AP scheduling is quiescent at this test stage. */
    /* Product disables AP scheduling before join, but an AP may still hold
     * the worker until its current yield completes. A fixed 512 BSP picks
     * can expire before that vCPU gets host time. Retain a bounded deadline. */
    uint64_t hz=cm_tsc_hz(),deadline=cm_rdtsc()+hz*2;
    unsigned limit=hz?10000000u:512u;
    for(unsigned i=0;worker && __atomic_load_n(&worker->id,__ATOMIC_ACQUIRE) && i<limit;i++) {
        if(hz && cm_rdtsc()>deadline)break;
        (void)cm_sched_run_until_idle(1);
        __asm__ volatile("pause" ::: "memory");
    }
    if(worker && worker->id)cm_panic("net RX worker did not stop");
    worker=NULL;
    kinfo("net irq rx stats: hw=%llu vnet_hw=%llu frames=%llu wakes=%llu sleeps=%llu idle=BLOCKED fallback=10ms/watchdog100ms",
        (unsigned long)cm_e1000_irq_count(),(unsigned long)cm_virtio_net_irq_count(),(unsigned long)cm_net_bg_irq_frames(),
        (unsigned long)__atomic_load_n(&irq_wakes,__ATOMIC_RELAXED),(unsigned long)cm_net_bg_sleeps());
}
uint64_t cm_net_bg_frames(void){return __atomic_load_n(&frames,__ATOMIC_RELAXED);}

uint64_t cm_net_bg_udp_queued(int id)
{return id>=0 && id<CM_MAX_IFACE?__atomic_load_n(&udp_queued[id],__ATOMIC_RELAXED):0;}

/* Narrow bootstrap hooks; transport/DHCP state machines remain unchanged. */
cm_error_t cm_net_dhcp_send(int id,uint8_t *frame,uint16_t length)
{
    cm_iface_t *iface=cm_if_get(id);
    if(!iface || !iface->up || iface->addr==CM_UDP_LO_ADDR)return CM_ERR_DEAD;
    if(!frame || length<42 || rd16(frame+12)!=CM_ETH_IP ||
       frame[23]!=CM_IPPROTO_UDP || rd32(frame+26)!=0 ||
       rd32(frame+30)!=UINT32_MAX || rd16(frame+34)!=68 || rd16(frame+36)!=67)return CM_ERR_INVAL;
    memset(frame,255,6);memcpy(frame+6,iface->mac,6);return tx(id,frame,length);
}
cm_error_t cm_net_boot_address(int id,uint32_t address,uint32_t mask,uint32_t gateway)
{
    cm_iface_t *iface=cm_if_get(id);if(!iface || !iface->up)return CM_ERR_DEAD;
    uint64_t f=guard();iface->addr=address;iface->netmask=mask;iface->gateway=gateway;
    memset(&peers[id],0,sizeof(peers[id]));unguard(f);return CM_OK;
}
