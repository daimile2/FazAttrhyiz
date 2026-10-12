#include "capmix/virtio_net.h"
#include "capmix/net.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>
/* OASIS Virtio legacy PCI: two split queues, separate 10-byte net header
 * descriptors, no mergeable buffers or checksum/GSO offload. */
static _Atomic int available;
static int iface_id=-1;
#ifndef CAPMIX_HOSTED
#define RX_BUFFERS 8u
#define NET_HEADER 10u
struct descriptor { uint64_t address;uint32_t length;uint16_t flags,next; };
_Static_assert(sizeof(struct descriptor)==16,"virtio descriptor ABI");
struct queue {
    uint8_t *memory;
    struct descriptor *desc;
    volatile uint16_t *avail,*used;
    uint16_t size,ai,ui;
};
static struct queue rxq,txq;
static uint8_t *rx_buffer[RX_BUFFERS],*tx_buffer;
static unsigned rx_count;
static uint16_t port,pci_command;
static uint32_t pci_address,features;
static volatile unsigned busy;
static int attempted;
static uint8_t in8(uint16_t p) {uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static uint16_t in16(uint16_t p) {uint16_t v;__asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p));return v;}
static uint32_t in32(uint16_t p) {uint32_t v;__asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p));return v;}
static void out8(uint16_t p,uint8_t v) {__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p):"memory");}
static void out16(uint16_t p,uint16_t v) {__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p):"memory");}
static void out32(uint16_t p,uint32_t v) {__asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p):"memory");}
static uint32_t config(uint32_t address,unsigned offset)
{out32(0xcf8,address|(offset&~3u));return in32(0xcfc);}
static void command_write(uint16_t command)
{out32(0xcf8,pci_address|4);out16(0xcfc,command);}
static int discover(void)
{
    int modern=0;
    for(unsigned bus=0;bus<256;bus++)for(unsigned slot=0;slot<32;slot++) {
        uint32_t address=0x80000000u|(bus<<16)|(slot<<11);
        if((config(address,0)&65535)==65535)continue;
        unsigned functions=(config(address,12)&0x800000)?8:1;
        for(unsigned f=0;f<functions;f++) {
            uint32_t a=address|(f<<8),id=config(a,0);
            if((id&65535)!=0x1af4)continue;
            if((id>>16)==0x1041)modern++;
            if((id>>16)!=0x1000)continue;
            uint32_t bar=config(a,16);
            if((config(a,8)&255)!=0 || !(bar&1) || !(bar&~3u) || (bar&~3u)>0xffc0)continue;
            /* Config offset 20 is the non-MSI-X legacy layout. */
            unsigned cap=(config(a,4)&0x100000)?config(a,0x34)&0xfc:0;
            int msix=0;
            for(unsigned guard=0;cap>=0x40 && cap<=0xfc && guard<48;guard++) {
                uint32_t c=config(a,cap);
                if((c&255)==0x11 && (c&0x80000000u))msix=1;
                cap=(c>>8)&0xfc;
            }
            if(msix)continue;
            pci_address=a;port=(uint16_t)(bar&~3u);pci_command=(uint16_t)config(a,4);
            kinfo("virtio-net PCI: %u:%u.%u legacy IO=%x",bus,slot,f,port);
            return 1;
        }
    }
    kinfo("virtio-net skipped: %s",modern?"modern-only PCI transport unsupported":"no supported PCI device");
    return 0;
}
static uint64_t physical(const void *p) {return (uint64_t)(uintptr_t)p-cm_hhdm();}
static int reset(void)
{
    out8(port+18,0);
    for(unsigned i=0;i<100000;i++)if(!in8(port+18))return 1;
    return 0;
}
static void stop(void)
{
    available=0;
    cm_iface_t *iface=cm_if_get(iface_id);if(iface)iface->up=0;
    int ack=reset();command_write((uint16_t)((pci_command|0x400)&~4u));
    if(ack) {
        if(rxq.memory)cm_page_free(rxq.memory,2);
        if(txq.memory)cm_page_free(txq.memory,2);
        if(tx_buffer)cm_page_free(tx_buffer,0);
        for(unsigned i=0;i<RX_BUFFERS;i++)if(rx_buffer[i])cm_page_free(rx_buffer[i],0);
        memset(&rxq,0,sizeof(rxq));memset(&txq,0,sizeof(txq));
        memset(rx_buffer,0,sizeof(rx_buffer));tx_buffer=NULL;
        command_write(pci_command);
    }
    kinfo("virtio-net rollback: reset=%s DMA stopped pages=%s",ack?"ok":"timeout",ack?"released":"quarantined");
}
static cm_error_t queue_init(struct queue *q,unsigned index)
{
    out16(port+14,(uint16_t)index);q->size=in16(port+12);
    if(q->size<2 || q->size>256 || (q->size&(q->size-1)))return CM_ERR_NOTSUP;
    q->memory=cm_page_alloc(2);
    if(!q->memory || physical(q->memory)>>44)return CM_ERR_NOMEM;
    memset(q->memory,0,4*CM_PAGE_SIZE);
    q->desc=(void *)q->memory;q->avail=(void *)(q->memory+16*q->size);
    q->used=(void *)(q->memory+CM_ALIGN_UP(16*q->size+6+2*q->size,4096));
    q->avail[0]=1; /* no interrupts; PCI INTx disabled too */
    out32(port+8,(uint32_t)(physical(q->memory)>>12));
    return in32(port+8)==(uint32_t)(physical(q->memory)>>12)?CM_OK:CM_ERR_IO;
}
static void publish(struct queue *q,uint16_t head)
{
    q->avail[2+(q->ai%q->size)]=head;
    __atomic_thread_fence(__ATOMIC_RELEASE);q->avail[1]=++q->ai;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
static int link_up(void) {return !(features&(1u<<16)) || (in16(port+26)&1);}
static uint64_t guard(void)
{
    uint64_t flags=cm_irq_save();
    while(__sync_lock_test_and_set(&busy,1u))__asm__ volatile("pause");
    return flags;
}
static void unguard(uint64_t flags) {__sync_lock_release(&busy);cm_irq_restore(flags);}
#endif
cm_error_t cm_virtio_net_init(void)
{
#ifdef CAPMIX_HOSTED
    kinfo("virtio-net skipped: hosted has no PCI/DMA");return CM_ERR_NOTSUP;
#else
    if(available)return CM_OK;
    if(attempted)return CM_ERR_DEAD;
    attempted=1;
    uint64_t flags=guard();
    if(!discover()){unguard(flags);return CM_ERR_NOENT;}
    command_write((uint16_t)(pci_command|5|0x400));
    if(!reset()){stop();unguard(flags);return CM_ERR_IO;}
    out8(port+18,1);out8(port+18,3);
    uint32_t offered=in32(port);
    if(!(offered&(1u<<5)) || !cm_hhdm()) {
        kinfo("virtio-net skipped: MAC feature/HHDM unavailable");stop();unguard(flags);return CM_ERR_NOTSUP;
    }
    features=offered&((1u<<5)|(1u<<16));out32(port+4,features);
    uint8_t mac[6];unsigned nonzero=0;
    for(unsigned i=0;i<6;i++){mac[i]=in8(port+20+i);nonzero|=mac[i];}
    cm_error_t err=(!nonzero || (mac[0]&1))?CM_ERR_INVAL:queue_init(&rxq,0);
    if(!err)err=queue_init(&txq,1);
    if(!err) {
        rx_count=CM_MIN(RX_BUFFERS,rxq.size/2u);tx_buffer=cm_page_alloc(0);
        if(!tx_buffer)err=CM_ERR_NOMEM;
        for(unsigned i=0;!err && i<rx_count;i++) {
            rx_buffer[i]=cm_page_alloc(0);
            if(!rx_buffer[i]){err=CM_ERR_NOMEM;break;}
            memset(rx_buffer[i],0,CM_PAGE_SIZE);
            rxq.desc[2*i]=(struct descriptor){physical(rx_buffer[i]),NET_HEADER,3,(uint16_t)(2*i+1)};
            rxq.desc[2*i+1]=(struct descriptor){physical(rx_buffer[i]+64),CM_PKT_MAX,2,0};
            publish(&rxq,(uint16_t)(2*i));
        }
    }
    if(err){kinfo("virtio-net skipped: initialization error=%d",err);stop();unguard(flags);return err;}
    out8(port+18,7);
    if((in8(port+18)&0x87)!=7){stop();unguard(flags);return CM_ERR_IO;}
    iface_id=cm_if_add("vnet0",0x0a00020fu);
    if(iface_id<0){stop();unguard(flags);return CM_ERR_NOSPC;}
    cm_iface_t *iface=cm_if_get(iface_id);
    memcpy(iface->mac,mac,6);iface->netmask=0xffffff00u;iface->gateway=0x0a000202u;
    iface->up=link_up();available=1;out16(port+16,0);
    kinfo("virtio-net ready: RXq=%u TXq=%u rx_buffers=%u link=%s MAC=%x:%x:%x:%x:%x:%x polling INTx=off",
          rxq.size,txq.size,rx_count,iface->up?"up":"down",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    kinfo("virtio-net ip: vnet0=10.0.2.15/24 gateway=10.0.2.2 status_feature=%u",!!(features&(1u<<16)));
    unguard(flags);return CM_OK;
#endif
}
int cm_virtio_net_available(void) {return available;}
int cm_virtio_net_ifid(void) {return iface_id;}
cm_error_t cm_virtio_net_transmit(const uint8_t *frame,uint16_t length)
{
    if(!frame || length<14 || length>CM_PKT_MAX)return CM_ERR_INVAL;
#ifdef CAPMIX_HOSTED
    return CM_ERR_NOTSUP;
#else
    uint64_t flags=guard();cm_error_t err=CM_OK;
    if(!available){unguard(flags);return CM_ERR_DEAD;}
    cm_iface_t *iface=cm_if_get(iface_id);iface->up=link_up();
    if(!iface->up){unguard(flags);return CM_ERR_DEAD;}
    memset(tx_buffer,0,64+CM_PKT_MAX);memcpy(tx_buffer+64,frame,length);
    txq.desc[0]=(struct descriptor){physical(tx_buffer),NET_HEADER,1,1};
    txq.desc[1]=(struct descriptor){physical(tx_buffer+64),CM_MAX(length,60u),0,0};
    publish(&txq,0);out16(port+16,1);
    unsigned spin;
    for(spin=0;spin<20000000;spin++) {
        if(__atomic_load_n(&txq.used[1],__ATOMIC_ACQUIRE)!=txq.ui)break;
        if(!(spin&4095))(void)in8(port+19);
        __asm__ volatile("pause" ::: "memory");
    }
    if(spin==20000000)err=CM_ERR_IO;
    else {
        volatile uint32_t *entry=(void *)((volatile uint8_t *)txq.used+4+8*(txq.ui%txq.size));
        if(txq.used[1]!=(uint16_t)(txq.ui+1) || entry[0]!=0)err=CM_ERR_IO;
        else {txq.ui++;__sync_fetch_and_add(&cm_if_get(iface_id)->tx,1);}
    }
    (void)in8(port+19);
    if(err){kerr("virtio-net TX timeout/invalid completion");stop();}
    unguard(flags);return err;
#endif
}
cm_error_t cm_virtio_net_receive(uint8_t *frame,uint16_t *length)
{
    if(!frame || !length)return CM_ERR_INVAL;
#ifdef CAPMIX_HOSTED
    return CM_ERR_NOTSUP;
#else
    uint64_t flags=guard();
    if(!available){unguard(flags);return CM_ERR_DEAD;}
    uint16_t count=(uint16_t)(__atomic_load_n(&rxq.used[1],__ATOMIC_ACQUIRE)-rxq.ui);
    if(!count) {
        static unsigned empty_polls;
        if(!(empty_polls++&1023) && (in8(port+19)&2))cm_if_get(iface_id)->up=link_up();
        unguard(flags);return CM_ERR_AGAIN;
    }
    volatile uint32_t *entry=(void *)((volatile uint8_t *)rxq.used+4+8*(rxq.ui%rxq.size));
    uint32_t head=entry[0],bytes=entry[1];
    if(count>rx_count || head>=2*rx_count || (head&1) || bytes<NET_HEADER+14 || bytes>NET_HEADER+CM_PKT_MAX) {
        kerr("virtio-net RX invalid completion");stop();unguard(flags);return CM_ERR_IO;
    }
    uint8_t *buffer=rx_buffer[head/2];
    cm_error_t err=CM_OK;
    if((buffer[0]&1) || buffer[1])err=CM_ERR_NOTSUP; /* offload was not negotiated */
    else {*length=(uint16_t)(bytes-NET_HEADER);memcpy(frame,buffer+64,*length);}
    rxq.ui++;memset(buffer,0,NET_HEADER);publish(&rxq,(uint16_t)head);out16(port+16,0);(void)in8(port+19);
    unguard(flags);return err;
#endif
}
