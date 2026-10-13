#include "capmix/net.h"
#include "capmix/virtio_net.h"
#include "capmix/e1000.h"
#include "capmix/arch.h"
#include "capmix/sched.h"
#include "capmix/klog.h"
#include <string.h>
/* One scheduler-owned consumer for both hardware rings. No timer/IRQ changes.
 * Driver guards serialize DMA operations; neighbor guard never encloses TX. */
struct neighbor {uint32_t addr;uint8_t mac[6];uint64_t asked;};
static struct neighbor peers[CM_MAX_IFACE];
static volatile unsigned peer_lock;
static cm_thread_t *worker;
static cm_task_t *worker_task;
static unsigned running;
static uint64_t frames,udp_queued[CM_MAX_IFACE];
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
static void ingress(int id,uint8_t *packet,uint16_t n)
{
    cm_iface_t *iface=cm_if_get(id);if(!iface || !iface->up || n<14)return;
    static const uint8_t broadcast[6]={255,255,255,255,255,255};
    if(memcmp(packet,iface->mac,6) && memcmp(packet,broadcast,6))return;
    if(rd16(packet+12)==CM_ETH_IP && n>=34 && packet[23]==CM_IPPROTO_UDP) {
        if(cm_udp_inject(id,packet,n)==CM_OK)__atomic_fetch_add(&udp_queued[id],1,__ATOMIC_RELAXED);
        return;
    }
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
    CM_UNUSED(arg);uint8_t packet[CM_PKT_MAX];
    while(__atomic_load_n(&running,__ATOMIC_ACQUIRE)) {
        for(int id=0;id<CM_MAX_IFACE;id++) {
            if(!cm_if_get(id))continue;
            for(unsigned budget=0;budget<16;budget++) {
                uint16_t bytes=0;cm_error_t e=rx(id,packet,&bytes);
                if(e==CM_ERR_AGAIN || e==CM_ERR_DEAD || e==CM_ERR_NOTSUP)break;
                if(e)continue;
                __atomic_fetch_add(&frames,1,__ATOMIC_RELAXED);ingress(id,packet,bytes);
            }
        }
        cm_sched_yield();
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
    memset(peers,0,sizeof(peers));__atomic_store_n(&running,1,__ATOMIC_RELEASE);
    worker=cm_thread_create(worker_task,background,NULL,CM_QOS_USER_INTERACTIVE);
    if(!worker){__atomic_store_n(&running,0,__ATOMIC_RELEASE);return CM_ERR_NOSPC;}
    kinfo("net bg RX started: scheduler thread tid=%llu budget=16/interface IRQ=off",
          (unsigned long)worker->id);return CM_OK;
}
void cm_net_bg_stop(void)
{
    __atomic_store_n(&running,0,__ATOMIC_RELEASE);
    /* Debug lifecycle only: AP scheduling is quiescent at this test stage. */
    for(unsigned i=0;worker && worker->id && i<512;i++)(void)cm_sched_run_until_idle(1);
    if(worker && worker->id)cm_panic("net RX worker did not stop");
    worker=NULL;
}
uint64_t cm_net_bg_frames(void){return __atomic_load_n(&frames,__ATOMIC_RELAXED);}

uint64_t cm_net_bg_udp_queued(int id)
{return id>=0 && id<CM_MAX_IFACE?__atomic_load_n(&udp_queued[id],__ATOMIC_RELAXED):0;}

/* Narrow bootstrap hooks; the RX worker/scheduling loop remains unchanged. */
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
