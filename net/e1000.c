#include "capmix/e1000.h"
#include "capmix/net.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>
/* Original implementation: Intel 8254x legacy descriptors; 82574 legacy
 * mode uses the same descriptor ABI. No offload, MSI, IRQ or NCQ-like queues. */
static int available,iface_id=-1;
#ifndef CAPMIX_HOSTED
#define N 16u
struct rxdesc {uint64_t address;uint16_t length,checksum;uint8_t status,errors;uint16_t special;};
struct txdesc {uint64_t address;uint16_t length;uint8_t cso,command,status,css;uint16_t special;};
_Static_assert(sizeof(struct rxdesc)==16 && sizeof(struct txdesc)==16,"e1000 DMA ABI");
static volatile uint32_t *mmio;
static uint32_t pci;
static uint16_t saved_command,device;
static uint8_t *rings,*tx_buffer,*rx_buffer[N];
static volatile struct rxdesc *rx;
static volatile struct txdesc *tx;
static unsigned rx_next,tx_next,busy;
static uint32_t reg(unsigned offset) {return mmio[offset/4];}
static void put(unsigned offset,uint32_t value) {mmio[offset/4]=value;}
static uint64_t physical(const void *p) {return (uint64_t)(uintptr_t)p-cm_hhdm();}
static uint32_t config(uint32_t a,unsigned offset)
{
    uint32_t address=a|(offset&~3u),v;
    __asm__ volatile("outl %0,%1"::"a"(address),"Nd"((uint16_t)0xcf8));
    __asm__ volatile("inl %1,%0":"=a"(v):"Nd"((uint16_t)0xcfc));return v;
}
static void command(uint16_t value)
{
    uint32_t address=pci|4;
    __asm__ volatile("outl %0,%1"::"a"(address),"Nd"((uint16_t)0xcf8));
    __asm__ volatile("outw %0,%1"::"a"(value),"Nd"((uint16_t)0xcfc):"memory");
}
static int reset(void)
{
    put(0xd8,UINT32_MAX);put(0x100,0);put(0x400,0);(void)reg(8);
    put(0,reg(0)|(1u<<26));
    for(unsigned i=0;i<100000;i++)if(!(reg(0)&(1u<<26)))return 1;
    return 0;
}
static void stop(void)
{
    available=0;cm_iface_t *iface=cm_if_get(iface_id);if(iface)iface->up=0;
    int ack=reset();command((uint16_t)((saved_command|0x400)&~4u));
    int disabled=!(config(pci,4)&4);
    if(ack && disabled) {
        if(rings)cm_page_free(rings,0);
        if(tx_buffer)cm_page_free(tx_buffer,0);
        for(unsigned i=0;i<N;i++)if(rx_buffer[i])cm_page_free(rx_buffer[i],0);
        rings=tx_buffer=NULL;memset(rx_buffer,0,sizeof(rx_buffer));rx=NULL;tx=NULL;
    }
    kinfo("e1000 rollback: reset=%s DMA=%s pages=%s",ack?"ok":"timeout",disabled?"off":"unconfirmed",ack&&disabled?"released":"quarantined");
}
static int discover(void)
{
    for(unsigned bus=0;bus<256;bus++)for(unsigned slot=0;slot<32;slot++) {
        uint32_t address=0x80000000u|(bus<<16)|(slot<<11);
        if((config(address,0)&65535)==65535)continue;
        unsigned nf=(config(address,12)&0x800000)?8:1;
        for(unsigned f=0;f<nf;f++) {
            uint32_t a=address|(f<<8),id=config(a,0);unsigned d=id>>16;
            if((id&65535)!=0x8086 || (d!=0x100e && d!=0x100f && d!=0x10d3) || (config(a,8)>>16)!=0x0200)continue;
            uint32_t bar=config(a,16);if((bar&7) || !(bar&~15u))continue;
            pci=a;device=(uint16_t)d;saved_command=(uint16_t)config(a,4);
            command((uint16_t)((saved_command|2|0x400)&~4u));
            uint64_t base=bar&~15u;
            mmio=cm_mmio_map(base&~4095ull,CM_ALIGN_UP((base&4095)+0x20000,4096)/4096);
            if(!mmio)return 0;
            mmio=(volatile uint32_t *)((volatile uint8_t *)mmio+(base&4095));
            kinfo("e1000 PCI: %u:%u.%u device=%x BAR0=%x model=%s",bus,slot,f,d,bar,d==0x10d3?"82574L/e1000e":"8254x");return 1;
        }
    }
    return 0;
}
static uint64_t guard(void)
{uint64_t flags=cm_irq_save();while(__sync_lock_test_and_set(&busy,1u))__asm__ volatile("pause");return flags;}
static void unguard(uint64_t flags) {__sync_lock_release(&busy);cm_irq_restore(flags);}
static int link(void) {return !!(reg(8)&2);}
#endif
cm_error_t cm_e1000_init(void)
{
#ifdef CAPMIX_HOSTED
    kinfo("e1000 skipped: hosted has no PCI/DMA");return CM_ERR_NOTSUP;
#else
    if(available)return CM_OK;
    static int attempted;if(attempted)return CM_ERR_DEAD;attempted=1;
    uint64_t flags=guard();
    if(!discover()){kinfo("e1000 skipped: no supported PCI NIC (82540/82545/82574)");unguard(flags);return CM_ERR_NOENT;}
    if(!reset()){stop();unguard(flags);return CM_ERR_IO;}
    uint32_t lo=reg(0x5400),hi=reg(0x5404);uint8_t mac[6];
    for(unsigned i=0;i<4;i++)mac[i]=(uint8_t)(lo>>(8*i));
    mac[4]=(uint8_t)hi;mac[5]=(uint8_t)(hi>>8);
    unsigned nonzero=0;for(unsigned i=0;i<6;i++)nonzero|=mac[i];
    if(!nonzero || (mac[0]&1) || !(hi&(1u<<31))) {
        kinfo("e1000 skipped: invalid/unavailable RAR0 MAC");stop();unguard(flags);return CM_ERR_NOTSUP;
    }
    rings=cm_page_alloc(0);tx_buffer=cm_page_alloc(0);
    cm_error_t e=rings&&tx_buffer?CM_OK:CM_ERR_NOMEM;
    for(unsigned i=0;!e && i<N;i++)if(!(rx_buffer[i]=cm_page_alloc(0)))e=CM_ERR_NOMEM;
    if(e){stop();unguard(flags);return e;}
    memset(rings,0,4096);memset(tx_buffer,0,4096);rx=(void *)rings;tx=(void *)(rings+512);
    for(unsigned i=0;i<N;i++){memset(rx_buffer[i],0,4096);rx[i].address=physical(rx_buffer[i]);tx[i].status=1;}
    for(unsigned i=0;i<128;i++)put(0x5200+4*i,0); /* no multicast hash filter */
    for(unsigned i=1;i<16;i++){put(0x5400+8*i,0);put(0x5404+8*i,0);}
    put(0x2800,(uint32_t)physical(rings));put(0x2804,(uint32_t)(physical(rings)>>32));
    put(0x2808,N*16);put(0x2810,0);put(0x2818,N-1);put(0x2820,0);put(0x282c,0);
    put(0x3800,(uint32_t)physical(rings+512));put(0x3804,(uint32_t)(physical(rings+512)>>32));
    put(0x3808,N*16);put(0x3810,0);put(0x3818,0);put(0x3820,0);put(0x382c,0);
    put(0x5000,0); /* RX checksum offload disabled */
    put(0x410,10|(8u<<10)|(6u<<20));
    if(device==0x10d3){put(0x18,reg(0x18)|(1u<<28));put(0x2828,reg(0x2828)|(1u<<25));put(0x3828,reg(0x3828)|(1u<<25));}
    put(0,reg(0)|(1u<<6)); /* SLU */
    rx_next=tx_next=0;__atomic_thread_fence(__ATOMIC_SEQ_CST);
    command((uint16_t)(saved_command|6|0x400));
    put(0x400,2|8|(15u<<4)|(64u<<12)); /* EN, PSP, CT, COLD */
    put(0x100,2|(1u<<15)|(1u<<26)); /* EN, BAM, SECRC; 2048B, no promiscuity */
    (void)reg(0xc0);put(0xd8,UINT32_MAX);
    iface_id=cm_if_add("enet0",0x0a00020fu);
    if(iface_id<0){stop();unguard(flags);return CM_ERR_NOSPC;}
    cm_iface_t *iface=cm_if_get(iface_id);memcpy(iface->mac,mac,6);
    iface->netmask=0xffffff00u;iface->gateway=0x0a000202u;iface->up=link();available=1;
    kinfo("e1000 ready: link=%s RX/TX=%u legacy DMA=2048 MAC=%x:%x:%x:%x:%x:%x polling IRQ=off",iface->up?"up":"down",N,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    kinfo("e1000 ip: enet0=10.0.2.15/24 gateway=10.0.2.2");unguard(flags);return CM_OK;
#endif
}
int cm_e1000_available(void) {return available;}
int cm_e1000_ifid(void) {return iface_id;}
cm_error_t cm_e1000_transmit(const uint8_t *frame,uint16_t length)
{
    if(!frame || length<14 || length>CM_PKT_MAX)return CM_ERR_INVAL;
#ifdef CAPMIX_HOSTED
    return CM_ERR_NOTSUP;
#else
    uint64_t flags=guard();if(!available){unguard(flags);return CM_ERR_DEAD;}
    cm_iface_t *iface=cm_if_get(iface_id);iface->up=link();
    if(!iface->up){unguard(flags);return CM_ERR_DEAD;}
    memset(tx_buffer,0,2048);memcpy(tx_buffer,frame,length);
    volatile struct txdesc *d=tx+tx_next;
    *d=(struct txdesc){.address=physical(tx_buffer),.length=(uint16_t)CM_MAX(length,60u),.command=1|2|8};
    __atomic_thread_fence(__ATOMIC_SEQ_CST);put(0x3818,(tx_next+1)%N);
    uint64_t hz=cm_tsc_hz(),end=hz?cm_rdtsc()+hz:UINT64_MAX;unsigned n;
    for(n=0;n<2000000 && cm_rdtsc()<end;n++){if(d->status&1)break;__asm__ volatile("pause");}
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cm_error_t e=CM_OK;
    if(!(d->status&1) || (d->status&0x0e)){kerr("e1000 TX timeout/error status=%x",d->status);stop();e=CM_ERR_IO;}
    else {tx_next=(tx_next+1)%N;__sync_fetch_and_add(&iface->tx,1);}
    unguard(flags);return e;
#endif
}
cm_error_t cm_e1000_receive(uint8_t *frame,uint16_t *length)
{
    if(!frame || !length)return CM_ERR_INVAL;
#ifdef CAPMIX_HOSTED
    return CM_ERR_NOTSUP;
#else
    uint64_t flags=guard();if(!available){unguard(flags);return CM_ERR_DEAD;}
    cm_iface_t *iface=cm_if_get(iface_id);iface->up=link();
    volatile struct rxdesc *d=rx+rx_next;
    if(!(d->status&1)){unguard(flags);return CM_ERR_AGAIN;}
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    unsigned bytes=d->length;
    cm_error_t e=CM_OK;
    if(d->address!=physical(rx_buffer[rx_next]) || bytes>2048) {
        kerr("e1000 RX invalid descriptor: status=%x len=%u",d->status,bytes);stop();unguard(flags);return CM_ERR_IO;
    }
    if(d->errors || !(d->status&2) || bytes<14 || bytes>CM_PKT_MAX){__sync_fetch_and_add(&iface->drop,1);e=CM_ERR_INVAL;}
    else {memcpy(frame,rx_buffer[rx_next],bytes);*length=(uint16_t)bytes;}
    d->length=0;d->errors=0;d->status=0;
    __atomic_thread_fence(__ATOMIC_RELEASE);put(0x2818,rx_next);rx_next=(rx_next+1)%N;
    unguard(flags);return e;
#endif
}
