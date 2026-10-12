/* Original one-shot VMCS setup, Intel SDM vol.3C chapters VM-entry/VM-exit.
 * No EPT: this trusted VMCALL-only stub shares the current kernel CR3. */
#include "capmix/virt_hw.h"
#include "capmix/arch.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include <string.h>
#ifndef CAPMIX_HOSTED
extern int cm_hw_rdmsr(uint32_t,uint64_t *);
extern int cm_hw_wrmsr(uint32_t,uint64_t);
struct dt { uint16_t limit; uint64_t base; } __attribute__((packed));
static bool write_field(cm_guest_result_t *r,uint64_t field,uint64_t value)
{
    int code=cm_hw_vmwrite(field,value);
    if (!code) return true;
    r->instruction_status=code; r->failed_field=field;
    if (code==2) (void)cm_hw_vmread(0x4400,&r->instruction_error);
    return false;
}
#define W(f,v) do { if (!write_field(r,(f),(v))) goto cleanup; } while (0)
static bool control(uint32_t msr,uint32_t wanted,uint32_t forbidden,uint32_t *out)
{
    uint64_t cap;
    if (cm_hw_rdmsr(msr,&cap)) return false;
    *out=(wanted|(uint32_t)cap)&(uint32_t)(cap>>32);
    bool supported=(*out&wanted)==wanted && !(*out&forbidden);
    if (!supported) kinfo("phase9: control unsupported msr=%x capability=%llx selected=%x wanted=%x forbidden=%x",msr,(unsigned long)cap,*out,wanted,forbidden);
    return supported;
}
bool cm_vmcs_guest_once(uint64_t basic,unsigned mode,cm_guest_result_t *r)
{
    bool ok=false,current=false;
    uint64_t phys=0,cr0,cr3,cr4,dr7,fs,gs,efer,pat,debug=0,syscs,sysesp,syseip;
    uint16_t selectors[7],ldtr;
    struct dt gdt,idt;
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    __asm__ volatile("mov %%cr3,%0":"=r"(cr3));
    cr4=cm_read_cr4();
    __asm__ volatile("mov %%dr7,%0":"=r"(dr7));
    __asm__ volatile("sgdt %0":"=m"(gdt));
    __asm__ volatile("sidt %0":"=m"(idt));
    __asm__ volatile("mov %%es,%0":"=r"(selectors[0]));
    __asm__ volatile("mov %%cs,%0":"=r"(selectors[1]));
    __asm__ volatile("mov %%ss,%0":"=r"(selectors[2]));
    __asm__ volatile("mov %%ds,%0":"=r"(selectors[3]));
    __asm__ volatile("mov %%fs,%0":"=r"(selectors[4]));
    __asm__ volatile("mov %%gs,%0":"=r"(selectors[5]));
    __asm__ volatile("str %0":"=r"(selectors[6]));
    __asm__ volatile("sldt %0":"=r"(ldtr));
    r->stage="host descriptor checks";
    /* VM-exit gives GDTR/IDTR limit=ffff and clears LDTR. Restore original
     * tables later; this kernel uses no LDT, so do not accept a live one. */
    if (ldtr || selectors[1]!=8 || selectors[2]!=16 || !selectors[6] ||
        (selectors[6]&7) || (unsigned)selectors[6]+15>gdt.limit) goto cleanup;
    for (unsigned i=0;i<7;i++) if (selectors[i]&7) goto cleanup;
    uint64_t lo,hi;
    memcpy(&lo,(void *)(uintptr_t)(gdt.base+selectors[6]),8);
    memcpy(&hi,(void *)(uintptr_t)(gdt.base+selectors[6]+8),8);
    uint64_t trbase=((lo>>16)&0xffffff)|((lo>>32)&0xff000000)|(hi<<32);
    uint32_t trlimit=(uint32_t)((lo&0xffff)|((lo>>32)&0xf0000));
    r->stage="host MSR snapshot";
    if (cm_hw_rdmsr(0xc0000100,&fs) || cm_hw_rdmsr(0xc0000101,&gs) ||
        cm_hw_rdmsr(0xc0000080,&efer) || cm_hw_rdmsr(0x277,&pat) ||
        cm_hw_rdmsr(0x1d9,&debug) || cm_hw_rdmsr(0x174,&syscs) ||
        cm_hw_rdmsr(0x175,&sysesp) || cm_hw_rdmsr(0x176,&syseip)) goto cleanup;
    r->stage="control capabilities";
    uint32_t pin,proc,entry,exit;
    bool true_controls=(basic&(1ull<<55))!=0;
    /* No secondary, TPR-shadow, bitmap, virtual NMI or timer dependencies. */
    if (!control(true_controls ? 0x48d : 0x481,0,(1u<<5)|(1u<<6)|(1u<<7),&pin) ||
        !control(true_controls ? 0x48e : 0x482,0,(1u<<21)|(1u<<25)|(1u<<28)|(1u<<31),&proc) ||
        !control(true_controls ? 0x490 : 0x484,(1u<<9)|(1u<<14)|(1u<<15),(1u<<10)|(1u<<11)|(1u<<13)|0xffff0000u,&entry) ||
        !control(true_controls ? 0x48f : 0x483,(1u<<9)|(1u<<19)|(1u<<21),
                 (1u<<22)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<28)|(1u<<29),&exit)) goto cleanup;
    r->stage="VMCS region allocation";
    r->vmcs_region=cm_page_alloc(0); r->guest_stack=cm_page_alloc(0);
    if (!r->vmcs_region || !r->guest_stack) goto cleanup;
    memset(r->vmcs_region,0,CM_PAGE_SIZE); memset(r->guest_stack,0,CM_PAGE_SIZE);
    *(uint32_t *)r->vmcs_region=(uint32_t)basic&0x7fffffff; /* shadow bit must be 0 */
    phys=(uint64_t)(uintptr_t)r->vmcs_region-cm_hhdm();
    if ((phys&4095) || ((basic&(1ull<<48)) && (phys>>32))) goto cleanup;
    r->stage="VMCLEAR initial";
    r->instruction_status=cm_hw_vmclear(&phys); if (r->instruction_status) goto cleanup;
    r->stage="VMPTRLD";
    r->instruction_status=cm_hw_vmptrld(&phys); if (r->instruction_status) goto cleanup;
    current=true;
    if (mode==1) { r->stage="injected abort after VMPTRLD"; goto cleanup; }
    r->stage="VMWRITE controls/state";
    W(0x4000,pin); W(0x4002,proc); W(0x4004,UINT32_MAX);
    W(0x4006,0); W(0x4008,0); W(0x400a,0);
    W(0x400c,exit); W(0x400e,0); W(0x4010,0);
    W(0x4012,entry); W(0x4014,0); W(0x4016,0); W(0x4018,0); W(0x401a,0);
    W(0x6000,0); W(0x6002,0); W(0x6004,cr0); W(0x6006,cr4);
    W(0x2800,UINT64_MAX); W(0x2802,debug); W(0x2804,pat); W(0x2806,efer);
    W(0x2c00,pat); W(0x2c02,efer);
    W(0x6800,cr0); W(0x6802,cr3); W(0x6804,cr4);
    W(0x6816,gdt.base); W(0x6818,idt.base); W(0x4810,gdt.limit); W(0x4812,idt.limit);
    /* Guest segments use the already valid kernel descriptors. */
    uint16_t guest_sel[8]={16,8,16,16,0,0,0,selectors[6]};
    uint32_t ar[8]={0xc093,0xa09b,0xc093,0xc093,0x10000,0x10000,0x10000,0x8b};
    for (unsigned i=0;i<8;i++) {
        W(0x800+2*i,guest_sel[i]); W(0x4800+2*i,i==7 ? trlimit : i>=4 ? 0 : UINT32_MAX);
        W(0x4814+2*i,ar[i]); W(0x6806+2*i,i==7 ? trbase : 0);
    }
    W(0x681a,0x400); W(0x681c,(uint64_t)(uintptr_t)r->guest_stack+4096-16);
    W(0x681e,(uint64_t)(uintptr_t)cm_guest_vmcall);
    W(0x6820,mode==2 ? 0 : 2); /* invalid RFLAGS exercises late entry failure */
    W(0x6822,0); W(0x4824,0); W(0x4826,0); W(0x482a,syscs);
    W(0x6824,sysesp); W(0x6826,syseip);
    for (unsigned i=0;i<7;i++) W(0xc00+2*i,selectors[i]);
    W(0x6c00,cr0); W(0x6c02,cr3); W(0x6c04,cr4);
    W(0x6c06,fs); W(0x6c08,gs); W(0x6c0a,trbase);
    W(0x6c0c,gdt.base); W(0x6c0e,idt.base);
    W(0x4c00,syscs); W(0x6c10,sysesp); W(0x6c12,syseip);
    r->vmcs_ready=true;
    kinfo("phase9: vmcs init ok VMPTRLD ok guest=64-bit shared host CR3 no EPT");
    r->stage="VMLAUNCH";
    r->instruction_status=cm_hw_vmlaunch_once();
    if (r->instruction_status==4) {
        r->entered=true;
        if (cm_hw_vmread(0x4402,&r->exit_reason) || cm_hw_vmread(0x6400,&r->qualification) ||
            cm_hw_vmread(0x681e,&r->guest_rip)) { r->stage="VMREAD exit"; goto cleanup; }
        r->real_exit=(r->exit_reason&(1ull<<31))==0;
        ok=r->real_exit && (r->exit_reason&0xffff)==18;
    } else if (r->instruction_status==2) (void)cm_hw_vmread(0x4400,&r->instruction_error);
cleanup:
    if (current) {
        int code=cm_hw_vmclear(&phys); r->vmcs_cleared=code==0;
        if (code) { r->stage="VMCLEAR cleanup failure (VMXOFF required)"; r->instruction_status=code; ok=false; }
    }
    /* VM-exit changes descriptor table limits even with identical bases. */
    if (r->entered) {
        /* VM-exit fixes DR7=0x400 and can clear DEBUGCTL; no host fields exist. */
        __asm__ volatile("mov %0,%%dr7"::"r"(dr7):"memory");
        if (cm_hw_wrmsr(0x1d9,debug)) { r->stage="DEBUGCTL restore failed"; r->hardware.rolled_back=false; ok=false; }
        __asm__ volatile("lgdt %0"::"m"(gdt):"memory");
        __asm__ volatile("lidt %0"::"m"(idt):"memory");
    }
    /* Pages remain owned until outer VMXOFF has definitely succeeded. */
    return ok;
}
#endif
