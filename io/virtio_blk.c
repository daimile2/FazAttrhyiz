#include "capmix/virtio_blk.h"
#include "capmix/io.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>
/* Legacy PCI transport / split virtqueue per OASIS Virtio 1.2 sections
 * 2.7.2, 4.1.4.10 and 5.2. Single in-flight request, owned DMA bounce pages. */
static _Atomic int available;
static uint64_t capacity,requests,flushes;
#ifndef CAPMIX_HOSTED
struct descriptor { uint64_t address; uint32_t length; uint16_t flags,next; };
struct request { uint32_t type,reserved; uint64_t sector; };
_Static_assert(sizeof(struct descriptor)==16 && sizeof(struct request)==16,"virtio DMA ABI");
static uint16_t port,qsize,pci_command;
static uint32_t pci_address;
static uint8_t *queue_mem,*request_mem;
static struct descriptor *desc;
static volatile uint16_t *avail,*used;
static uint16_t avail_index,used_index;
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
static uint64_t physical(const void *p) {return (uint64_t)(uintptr_t)p-cm_hhdm();}
static int reset(void)
{
    out8(port+18,0);
    for(unsigned i=0;i<100000;i++) if(!in8(port+18))return 1;
    return 0;
}
static void stop(void)
{
    available=0;
    int ack=reset();
    command_write((uint16_t)((pci_command|0x400)&~4u)); /* DMA off before freeing */
    if(ack) {
        if(queue_mem)cm_page_free(queue_mem,2);
        if(request_mem)cm_page_free(request_mem,1);
        queue_mem=request_mem=NULL;
        command_write(pci_command);
    }
    kinfo("virtio-blk rollback: reset=%s DMA stopped pages=%s",ack?"ok":"timeout",ack?"released":"quarantined");
}
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
            if((id>>16)==0x1042)modern++;
            if((id>>16)!=0x1001)continue;
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
            kinfo("virtio-blk PCI: %u:%u.%u legacy IO=%x",bus,slot,f,port);
            return 1;
        }
    }
    kinfo("virtio-blk skipped: %s",modern?"modern-only PCI transport unsupported":"no supported PCI device");
    return 0;
}
static cm_error_t transfer(uint32_t type,uint64_t sector,uint32_t bytes,void *buffer)
{
    struct request *r=(void *)request_mem;
    volatile uint8_t *status=request_mem+16;
    uint8_t *data=request_mem+64;
    *r=(struct request){type,0,sector};*status=255;
    if(type==1)memcpy(data,buffer,bytes);
    desc[0]=(struct descriptor){physical(r),16,1,1};
    if(bytes) {
        desc[1]=(struct descriptor){physical(data),bytes,(uint16_t)(1|(type==0?2:0)),2};
        desc[2]=(struct descriptor){physical((const void *)status),1,2,0};
    } else desc[1]=(struct descriptor){physical((const void *)status),1,2,0};
    avail[2+(avail_index%qsize)]=0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    avail[1]=++avail_index;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    out16(port+16,0);
    unsigned spin;
    for(spin=0;spin<20000000;spin++) {
        if(__atomic_load_n(&used[1],__ATOMIC_ACQUIRE)!=used_index)break;
        if(!(spin&4095)) (void)in8(port+19); /* acknowledge and service PCI exit */
        __asm__ volatile("pause" ::: "memory");
    }
    if(spin==20000000) { kerr("virtio-blk timeout type=%u sector=%llu",type,(unsigned long)sector);stop();return CM_ERR_IO; }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    volatile uint32_t *entry=(volatile uint32_t *)((volatile uint8_t *)used+4+8*(used_index%qsize));
    uint32_t expected=type==0 ? bytes+1 : 1;
    uint8_t result_status=*status;
    if(used[1]!=(uint16_t)(used_index+1) || entry[0]!=0 || entry[1]<1 || entry[1]>expected ||
       result_status>2 || (!result_status && entry[1]!=expected)) {
        kerr("virtio-blk invalid used-ring completion");stop();return CM_ERR_IO;
    }
    used_index++;requests++;(void)in8(port+19);
    if(result_status)return result_status==2?CM_ERR_NOTSUP:CM_ERR_IO;
    if(type==0)memcpy(buffer,data,bytes);
    if(type==4)flushes++;
    return CM_OK;
}
#endif
cm_error_t cm_virtio_blk_init(void)
{
#ifdef CAPMIX_HOSTED
    kinfo("virtio-blk skipped: hosted has no PCI/DMA");return CM_ERR_NOTSUP;
#else
    if(available)return CM_OK;
    if(attempted)return CM_ERR_DEAD; /* failed/quarantined devices are not reinitialized */
    attempted=1;
    uint64_t flags=cm_irq_save();
    if(!discover()){cm_irq_restore(flags);return CM_ERR_NOENT;}
    command_write((uint16_t)(pci_command|5|0x400)); /* IO + bus master, INTx disabled */
    if(!reset()){stop();cm_irq_restore(flags);return CM_ERR_IO;}
    out8(port+18,1);out8(port+18,3);
    uint32_t offered=in32(port);
    if((offered&(1u<<5)) || !(offered&(1u<<9))) {
        kinfo("virtio-blk skipped: read-only or FLUSH unsupported features=%x",offered);
        stop();cm_irq_restore(flags);return CM_ERR_NOTSUP;
    }
    out32(port+4,1u<<9); /* FLUSH only; no indirect/event-index/packed ring */
    out16(port+14,0);qsize=in16(port+12);
    capacity=(uint64_t)in32(port+20)|((uint64_t)in32(port+24)<<32);
    if(qsize<3 || qsize>256 || (qsize&(qsize-1)) || capacity<CM_BLK_DISK_SECT || !cm_hhdm()) {
        kinfo("virtio-blk skipped: queue/capacity unsupported q=%u sectors=%llu",qsize,(unsigned long)capacity);
        stop();cm_irq_restore(flags);return CM_ERR_NOTSUP;
    }
    queue_mem=cm_page_alloc(2);request_mem=cm_page_alloc(1);
    if(!queue_mem || !request_mem || physical(queue_mem)>>44) {stop();cm_irq_restore(flags);return CM_ERR_NOMEM;}
    memset(queue_mem,0,4*CM_PAGE_SIZE);memset(request_mem,0,2*CM_PAGE_SIZE);
    desc=(void *)queue_mem;avail=(void *)(queue_mem+16*qsize);
    used=(void *)(queue_mem+CM_ALIGN_UP(16*qsize+6+2*qsize,4096));
    avail[0]=1;avail_index=used_index=0;
    out32(port+8,(uint32_t)(physical(queue_mem)>>12));
    if(in32(port+8)!=(uint32_t)(physical(queue_mem)>>12)){stop();cm_irq_restore(flags);return CM_ERR_IO;}
    out8(port+18,7);
    if ((in8(port+18)&0x87)!=7) {stop();cm_irq_restore(flags);return CM_ERR_IO;}
    available=1;
    kinfo("virtio-blk ready: sectors=%llu queue=%u DMA bounce=4096 FLUSH=1 polling INTx=off",
          (unsigned long)capacity,qsize);
    cm_irq_restore(flags);return CM_OK;
#endif
}
int cm_virtio_blk_available(void) {return available;}
uint64_t cm_virtio_blk_capacity(void) {return capacity;}
uint64_t cm_virtio_blk_requests(void) {return requests;}
uint64_t cm_virtio_blk_flushes(void) {return flushes;}
cm_error_t cm_virtio_blk_rw(uint32_t op,uint64_t sector,uint32_t count,void *buffer)
{
    if((op!=CM_OP_READ && op!=CM_OP_WRITE) || !buffer || !count || sector>=capacity || count>capacity-sector)return CM_ERR_INVAL;
#ifdef CAPMIX_HOSTED
    return CM_ERR_NOTSUP;
#else
    uint64_t flags=cm_irq_save();
    while(__sync_lock_test_and_set(&busy,1u))__asm__ volatile("pause");
    cm_error_t result=available?CM_OK:CM_ERR_DEAD;
    for(uint32_t off=0;!result && off<count;) {
        uint32_t n=CM_MIN(count-off,8u);
        result=transfer(op==CM_OP_READ?0:1,sector+off,n*512,(uint8_t *)buffer+(uint64_t)off*512);off+=n;
    }
    __sync_lock_release(&busy);cm_irq_restore(flags);return result;
#endif
}
cm_error_t cm_virtio_blk_flush(void)
{
#ifdef CAPMIX_HOSTED
    return CM_ERR_NOTSUP;
#else
    uint64_t flags=cm_irq_save();
    while(__sync_lock_test_and_set(&busy,1u))__asm__ volatile("pause");
    cm_error_t result=available?transfer(4,0,0,NULL):CM_ERR_DEAD;
    __sync_lock_release(&busy);cm_irq_restore(flags);return result;
#endif
}
