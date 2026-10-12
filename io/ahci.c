#include "capmix/ahci.h"
#include "capmix/io.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>
/* Original implementation of Intel AHCI 1.3.1. One SATA port, slot 0,
 * non-NCQ LBA48, owned DMA bounce pages, polling (no IRQ routing changes). */
static int available;
static uint64_t sectors;
#ifndef CAPMIX_HOSTED
static volatile uint32_t *hba,*port;
static uint32_t pci;
static uint16_t saved_command;
static uint8_t *control,*bounce;
static unsigned busy;
struct header {uint16_t flags,prdt;uint32_t transferred;uint64_t table;uint32_t reserved[4];};
struct prd {uint64_t address;uint32_t reserved,bytes;};
_Static_assert(sizeof(struct header)==32 && sizeof(struct prd)==16,"AHCI ABI");
static uint32_t config(uint32_t a,unsigned off)
{
    uint32_t v,aoff=a|(off&~3u);
    __asm__ volatile("outl %0,%1"::"a"(aoff),"Nd"((uint16_t)0xcf8));
    __asm__ volatile("inl %1,%0":"=a"(v):"Nd"((uint16_t)0xcfc));return v;
}
static void command(uint16_t v)
{
    uint32_t a=pci|4;
    __asm__ volatile("outl %0,%1"::"a"(a),"Nd"((uint16_t)0xcf8));
    __asm__ volatile("outw %0,%1"::"a"(v),"Nd"((uint16_t)0xcfc):"memory");
}
static uint64_t physical(void *p) {return (uint64_t)(uintptr_t)p-cm_hhdm();}
static int wait_clear(unsigned reg,uint32_t mask)
{
    uint64_t hz=cm_tsc_hz(), end=hz?cm_rdtsc()+hz:UINT64_MAX;
    for(unsigned i=0;i<10000000 && cm_rdtsc()<end;i++) {
        if(!(port[reg]&mask))return 1;
        __asm__ volatile("pause" ::: "memory");
    }
    return !(port[reg]&mask);
}
static int stop_engine(void)
{
    port[6]&=~1u;
    int ok=wait_clear(6,1u<<15);
    port[6]&=~(1u<<4);
    return wait_clear(6,1u<<14) && ok;
}
static void rollback(void)
{
    available=0;int stopped=port?stop_engine():1;
    command((uint16_t)((saved_command|0x400)&~4u));
    if(stopped) {
        if(control)cm_page_free(control,0);
        if(bounce)cm_page_free(bounce,0);
        control=bounce=NULL;
        /* Do not re-enable firmware DMA against our freed addresses. */
        if(port){port[0]=port[1]=port[2]=port[3]=0;port[5]=0;}
    }
    kinfo("ahci rollback: DMA=off engine=%s pages=%s",stopped?"stopped":"timeout",stopped?"released":"quarantined");
}
static cm_error_t transfer(uint8_t opcode,uint64_t lba,uint16_t count,void *buffer,unsigned bytes,int write)
{
    if(!wait_clear(8,0x88) || port[14] || port[13])return CM_ERR_BUSY;
    memset(control,0,1024);memset(control+1280,0,256);
    struct header *head=(void *)control;
    *head=(struct header){.flags=(uint16_t)(5|(write?1u<<6:0)),.prdt=bytes?1:0,.table=physical(control+1280)};
    uint8_t *fis=control+1280;
    fis[0]=0x27;fis[1]=0x80;fis[2]=opcode;fis[7]=0x40;
    fis[4]=(uint8_t)lba;fis[5]=(uint8_t)(lba>>8);fis[6]=(uint8_t)(lba>>16);
    fis[8]=(uint8_t)(lba>>24);fis[9]=(uint8_t)(lba>>32);fis[10]=(uint8_t)(lba>>40);
    fis[12]=(uint8_t)count;fis[13]=(uint8_t)(count>>8);
    if(bytes) {
        if(write)memcpy(bounce,buffer,bytes);
        struct prd *p=(void *)(fis+128);
        *p=(struct prd){physical(bounce),0,bytes-1};
    }
    port[4]=UINT32_MAX;port[12]=UINT32_MAX;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);port[14]=1;
    int done=wait_clear(14,1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if(!done || (port[4]&(1u<<30)) || (port[8]&1) || (bytes && head->transferred!=bytes)) {
        kerr("ahci command failed: cmd=%x lba=%llu IS=%x TFD=%x CI=%x bytes=%u/%u",
             opcode,(unsigned long)lba,port[4],port[8],port[14],head->transferred,bytes);
        rollback();return CM_ERR_IO;
    }
    if(bytes && !write)memcpy(buffer,bounce,bytes);
    return CM_OK;
}
static int discover(void)
{
    for(unsigned bus=0;bus<256;bus++)for(unsigned slot=0;slot<32;slot++) {
        uint32_t a=0x80000000u|(bus<<16)|(slot<<11);
        if((config(a,0)&65535)==65535)continue;
        unsigned nf=(config(a,12)&0x800000)?8:1;
        for(unsigned f=0;f<nf;f++) {
            uint32_t at=a|(f<<8);
            if((config(at,8)>>8)!=0x010601)continue;
            uint32_t bar=config(at,0x24);
            if((bar&7) || !(bar&~15u))continue; /* ABAR: 32-bit non-prefetchable memory */
            pci=at;saved_command=(uint16_t)config(at,4);
            command((uint16_t)((saved_command|2|0x400)&~4u));
            uint64_t base=bar&~15u;
            hba=cm_mmio_map(base&~4095ull,CM_ALIGN_UP((base&4095)+0x1100,4096)/4096);
            if(!hba)return 0;
            hba=(volatile uint32_t *)((volatile uint8_t *)hba+(base&4095));
            kinfo("ahci PCI: %u:%u.%u ABAR=%x",bus,slot,f,bar);return 1;
        }
    }
    return 0;
}
#endif
cm_error_t cm_ahci_init(void)
{
#ifdef CAPMIX_HOSTED
    kinfo("ahci skipped: hosted has no PCI/DMA");return CM_ERR_NOTSUP;
#else
    if(available)return CM_OK;
    static int attempted;if(attempted)return CM_ERR_DEAD;attempted=1;
    uint64_t flags=cm_irq_save();
    if(!discover()){kinfo("ahci skipped: no supported controller");cm_irq_restore(flags);return CM_ERR_NOENT;}
    /* BIOS/OS ownership handshake (CAP2.BOH), bounded; do not force takeover. */
    if(hba[9]&1) {
        hba[10]|=2;uint64_t hz=cm_tsc_hz(), end=hz?cm_rdtsc()+2*hz:UINT64_MAX;unsigned limit=20000000;
        while((hba[10]&0x11) && limit-- && cm_rdtsc()<end)__asm__ volatile("pause");
        if(hba[10]&0x11){kinfo("ahci skipped: firmware ownership timeout");rollback();cm_irq_restore(flags);return CM_ERR_BUSY;}
    }
    hba[1]=(hba[1]|(1u<<31))&~2u; /* AE on, global interrupt off */
    uint32_t implemented=hba[3];
    for(unsigned i=0;i<32;i++)if(implemented&(1u<<i)) {
        volatile uint32_t *p=hba+0x40+0x20*i;
        if((p[10]&0x0f0f)==0x0103 && p[9]==0x00000101) {
            port=p;kinfo("ahci SATA disk: port=%u",i);break;
        }
    }
    if(!port){kinfo("ahci skipped: no active SATA disk (ATAPI unsupported)");rollback();cm_irq_restore(flags);return CM_ERR_NOENT;}
    /* Quiesce all implemented ports before enabling controller bus mastering. */
    volatile uint32_t *selected=port;
    for(unsigned i=0;i<32;i++)if(implemented&(1u<<i)) {
        port=hba+0x40+0x20*i;
        if(!stop_engine() || port[14] || port[13]) {rollback();cm_irq_restore(flags);return CM_ERR_BUSY;}
    }
    port=selected;
    control=cm_page_alloc(0);bounce=cm_page_alloc(0);
    if(!control || !bounce || (!((hba[0]>>31)&1) && ((physical(control)>>32) || (physical(bounce)>>32)))) {
        rollback();cm_irq_restore(flags);return CM_ERR_NOMEM;
    }
    memset(control,0,4096);memset(bounce,0,4096);
    uint64_t cl=physical(control),fb=physical(control+1024);
    port[0]=(uint32_t)cl;port[1]=(uint32_t)(cl>>32);port[2]=(uint32_t)fb;port[3]=(uint32_t)(fb>>32);
    port[5]=0;port[4]=UINT32_MAX;port[12]=UINT32_MAX;
    command((uint16_t)(saved_command|6|0x400));port[6]|=(1u<<4)|1;
    uint16_t id[256];cm_error_t e=transfer(0xec,0,0,id,512,0);
    if(e){if(e==CM_ERR_BUSY)rollback();cm_irq_restore(flags);return e;}
    sectors=(uint64_t)id[100]|((uint64_t)id[101]<<16)|((uint64_t)id[102]<<32)|((uint64_t)id[103]<<48);
    int logical512=(id[106]&0xc000)!=0x4000 || !(id[106]&(1u<<12)) || (id[117]==256 && !id[118]);
    if(!(id[83]&(1u<<10)) || !(id[83]&(1u<<13)) || !logical512 || sectors<CM_BLK_DISK_SECT || sectors>0x1000000000000ull) {
        kinfo("ahci skipped: require LBA48/FLUSH_EXT/512B/capacity >=8MiB");rollback();cm_irq_restore(flags);return CM_ERR_NOTSUP;
    }
    available=1;kinfo("ahci ready: sectors=%llu logical=512 DMA=4096 slot=0 polling FLUSH_EXT=1",(unsigned long)sectors);
    cm_irq_restore(flags);return CM_OK;
#endif
}
int cm_ahci_available(void) {return available;}
cm_error_t cm_ahci_rw(uint32_t op,uint64_t lba,uint32_t count,void *buffer)
{
    if(!buffer || !count || (op!=CM_OP_READ && op!=CM_OP_WRITE) || lba>=sectors || count>sectors-lba)return CM_ERR_INVAL;
#ifdef CAPMIX_HOSTED
    return CM_ERR_NOTSUP;
#else
    uint64_t flags=cm_irq_save();
    while(__sync_lock_test_and_set(&busy,1u))__asm__ volatile("pause");
    cm_error_t e=available?CM_OK:CM_ERR_DEAD;
    for(unsigned off=0;!e && off<count;) {
        unsigned n=CM_MIN(count-off,8u);
        e=transfer(op==CM_OP_WRITE?0x35:0x25,lba+off,(uint16_t)n,(uint8_t *)buffer+off*512,n*512,op==CM_OP_WRITE);off+=n;
    }
    if(e==CM_ERR_BUSY)rollback();
    __sync_lock_release(&busy);cm_irq_restore(flags);return e;
#endif
}
cm_error_t cm_ahci_flush(void)
{
#ifdef CAPMIX_HOSTED
    return CM_ERR_NOTSUP;
#else
    uint64_t flags=cm_irq_save();while(__sync_lock_test_and_set(&busy,1u))__asm__ volatile("pause");
    cm_error_t e=available?transfer(0xea,0,0,NULL,0,0):CM_ERR_DEAD;
    if(e==CM_ERR_BUSY)rollback();
    __sync_lock_release(&busy);cm_irq_restore(flags);return e;
#endif
}
int cm_ahci_selftest(void)
{
    if(!available){kinfo("ahci selftest skipped: no supported disk");return 0;}
    /* Outside the reserved 8MiB COW volume; save and restore even on mismatch. */
    if(sectors<=CM_BLK_DISK_SECT){kinfo("ahci selftest skipped: no spare test sector");return 0;}
    static uint8_t saved[512],data[512],got[512];uint64_t lba=sectors-1;
    cm_error_t e=cm_ahci_rw(CM_OP_READ,lba,1,saved);if(e)return 1;
    for(unsigned i=0;i<512;i++)data[i]=(uint8_t)(i*37+17);
    e=cm_ahci_rw(CM_OP_WRITE,lba,1,data);
    if(!e)e=cm_ahci_flush();
    if(!e)e=cm_ahci_rw(CM_OP_READ,lba,1,got);
    int fail=e || memcmp(data,got,512);
    cm_error_t restore=cm_ahci_rw(CM_OP_WRITE,lba,1,saved);
    if(!restore)restore=cm_ahci_flush();
    if(!restore)restore=cm_ahci_rw(CM_OP_READ,lba,1,got);
    int restored=!restore && !memcmp(saved,got,512);
    if(!restored)fail=1;
    kinfo("ahci read/write selftest: %s bytes=512 restore=%s",fail?"FAIL":"PASS",restored?"verified":"failed");
    return fail;
}
