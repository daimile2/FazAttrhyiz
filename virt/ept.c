/* Bounded 4-KiB EPT and private four-level guest identity paging.
 * Intel SDM: EPTP, EPT permissions and IA32_VMX_EPT_VPID_CAP. */
#include "capmix/virt_hw.h"
#include "capmix/arch.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include <string.h>
#ifndef CAPMIX_HOSTED
extern int cm_hw_rdmsr(uint32_t,uint64_t *);
static uint64_t physical(const void *p) { return (uint64_t)(uintptr_t)p-cm_hhdm(); }
int cm_ept_invalidate(cm_guest_result_t *r)
{
    struct { uint64_t eptp,reserved; } __attribute__((aligned(16))) desc={r->invept_type==1 ? r->eptp : 0,0};
    int code=cm_hw_invept(r->invept_type,&desc);
    if (code) {
        r->instruction_status=code;
        kinfo("ept: INVEPT failed type=%u status=%d",r->invept_type,code);
    }
    return code;
}
bool cm_ept_prepare(cm_guest_result_t *r,unsigned mode)
{
    r->stage="EPT capabilities";
    uint64_t cap;
    if (cm_hw_rdmsr(0x48c,&cap)) return false;
    if (mode==6) { cap&=~((1ull<<25)|(1ull<<26)); kinfo("ept: injected missing INVEPT capabilities"); }
    if (!(cap&(1ull<<6)) || !(cap&(1ull<<14)) || !(cap&(1ull<<20)) ||
        !(cap&((1ull<<25)|(1ull<<26)))) {
        kinfo("ept: skipped capabilities=%llx need 4-level/WB/INVEPT single-or-all",(unsigned long)cap);
        return false;
    }
    r->invept_type=(cap&(1ull<<25)) ? 1 : 2;
    r->stage="EPT/private guest pages allocation";
    for (unsigned i=0;i<4;i++) {
        r->ept_tables[i]=cm_page_alloc(0);
        if (!r->ept_tables[i]) return false;
        memset(r->ept_tables[i],0,CM_PAGE_SIZE);
        if (mode==5 && i==1) { r->stage="injected partial EPT allocation abort"; return false; }
    }
    for (unsigned i=0;i<7;i++) {
        r->guest_pages[i]=cm_page_alloc(0);
        if (!r->guest_pages[i]) return false;
        memset(r->guest_pages[i],0,CM_PAGE_SIZE);
    }
    uint64_t *epml4=r->ept_tables[0],*epdpt=r->ept_tables[1],*epd=r->ept_tables[2],*ept=r->ept_tables[3];
    epml4[0]=physical(epdpt)|7; epdpt[0]=physical(epd)|7; epd[0]=physical(ept)|7;
    for (unsigned i=0;i<7;i++) {
        uint64_t permissions=i==0 ? (mode==4 ? 1 : 5) : 3;
        ept[i+1]=physical(r->guest_pages[i])|permissions|(6ull<<3);
    }
    /* GPA 0x1000 code, 0x2000 stack, 0x3000..6000 guest tables, 0x7000 GDT/TSS. */
    uint64_t *gpml4=r->guest_pages[2],*gpdpt=r->guest_pages[3],*gpd=r->guest_pages[4],*gpt=r->guest_pages[5];
    gpml4[0]=0x4000|0x23; gpdpt[0]=0x5000|0x23; gpd[0]=0x6000|0x23;
    for (unsigned i=1;i<=8;i++) gpt[i]=(uint64_t)i*4096|(i==1 ? 0x61 : 0x63);
    /* PTE[8] is present, but GPA 0x8000 has no EPT leaf. */
    static const uint8_t vmcall[]={0x0f,0x01,0xc1,0x0f,0x0b};
    static const uint8_t unmapped[]={0x48,0xb8,0x00,0x80,0,0,0,0,0,0,0x48,0x8b,0x00,0x0f,0x01,0xc1,0x0f,0x0b};
    memcpy(r->guest_pages[0],mode==3 ? unmapped : vmcall,mode==3 ? sizeof(unmapped) : sizeof(vmcall));
    uint64_t *gdt=r->guest_pages[6];
    gdt[1]=0x00af9b000000ffffull; gdt[2]=0x00cf93000000ffffull;
    gdt[3]=0x67|((uint64_t)0x7100<<16)|(0x8bull<<40); gdt[4]=0;
    ((uint8_t *)gdt)[0x166]=0x68; /* TSS I/O bitmap outside its limit. */
    r->guest_cr3=0x3000; r->guest_entry=0x1000;
    r->eptp=physical(epml4)|6|(3ull<<3); /* WB, four-level, A/D disabled. */
    r->ept_ready=true; r->stage="INVEPT initial EPT publish";
    if (cm_ept_invalidate(r)) return false;
    kinfo("ept init: 4-level 4KiB leaves mapped=7 code=RX data=RW/NX guest_cr3=0x3000 EPTP=%llx INVEPT type=%u",
          (unsigned long)r->eptp,r->invept_type);
    return true;
}
#endif
