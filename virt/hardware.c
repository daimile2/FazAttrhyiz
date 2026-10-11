/* Intel SDM VMX operation / AMD APM vol.2 SVM initialization.
 * BSP-only transient probe; never programs an irreversible firmware lock. */
#include "capmix/virt_hw.h"
#include "capmix/arch.h"
#include "capmix/mm.h"
#include <string.h>
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid":"=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d):"a"(leaf),"c"(0));
}
#ifndef CAPMIX_HOSTED
extern int cm_hw_rdmsr(uint32_t, uint64_t *);
extern int cm_hw_wrmsr(uint32_t, uint64_t);
extern int cm_hw_cr4(uint64_t);
extern int cm_hw_cr0(uint64_t);
extern int cm_hw_vmxon(const uint64_t *);
extern int cm_hw_vmxoff(void);
extern int cm_hw_test_ud(void);
extern void cm_hw_fault_6(void), cm_hw_fault_13(void);
extern const uint64_t cm_hw_fixups_begin[], cm_hw_fixups_end[];
static bool armed;
static unsigned fault_vector;
/* Raw frame: error, RIP, CS, RFLAGS, RSP, SS. Nonmatching faults still halt. */
void cm_hw_fault_recover(uint64_t vec, uint64_t *frame)
{
    if (armed && cm_cpu_id()==0 && frame[2]==0x08 && frame[0]==0 &&
        (vec==6 || vec==13)) {
        for (const uint64_t *p=cm_hw_fixups_begin;p<cm_hw_fixups_end;p+=2) {
            if (frame[1]==p[0]) { frame[1]=p[1]; fault_vector=(unsigned)vec; return; }
        }
    }
    cm_fault_isr(vec,frame[0],frame[1],(uint64_t)(uintptr_t)frame);
}
struct idtr { uint16_t limit; uint64_t base; } __attribute__((packed));
/* A private BSP IDT avoids rewriting the shared table while APs run. */
static uint8_t probe_idt[4096] __attribute__((aligned(16)));
static void probe_gate(unsigned vec, void (*fn)(void))
{
    uint64_t addr=(uint64_t)(uintptr_t)fn;
    uint64_t gate[2]={ (addr&0xffff) | (0x08ull<<16) | (1ull<<32) |
                      (0x8eull<<40) | ((addr&0xffff0000ull)<<32), addr>>32 };
    memcpy(probe_idt+vec*16,gate,16);
}
static uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0,%0":"=r"(v)); return v; }
#endif
cm_virt_hw_result_t cm_virt_hw_probe(bool abort_prepare)
{
    cm_virt_hw_result_t r={.rolled_back=true,.status=CM_VIRT_HW_UNSUPPORTED,.stage="cpuid"};
    uint32_t a,b,c,d,max;
    cpuid(0,&max,&b,&c,&d);
    if (max>=1) { cpuid(1,&a,&b,&c,&d); r.vmx=(c&(1u<<5))!=0; }
    cpuid(0x80000000,&max,&b,&c,&d);
    if (max>=0x80000001) { cpuid(0x80000001,&a,&b,&c,&d); r.svm=(c&(1u<<2))!=0; }
    if (!r.vmx && !r.svm) return r;
#ifdef CAPMIX_HOSTED
    (void)abort_prepare;
    r.status=CM_VIRT_HW_HOSTED;
    return r;
#else
    uint64_t flags=cm_irq_save();
    if (cm_cpu_id()!=0 || armed) { r.status=CM_VIRT_HW_POLICY; cm_irq_restore(flags); return r; }
    struct idtr idt;
    __asm__ volatile("sidt %0":"=m"(idt));
    if (!idt.base || idt.limit<14*16-1 || idt.limit>=sizeof(probe_idt)) { r.status=CM_VIRT_HW_INVALID; cm_irq_restore(flags); return r; }
    memcpy(probe_idt,(const void *)(uintptr_t)idt.base,(size_t)idt.limit+1);
    probe_gate(6,cm_hw_fault_6); probe_gate(13,cm_hw_fault_13);
    struct idtr local={.limit=idt.limit,.base=(uint64_t)(uintptr_t)probe_idt};
    fault_vector=0; armed=true;
    __asm__ volatile("lidt %0"::"m"(local):"memory");
    void *page=NULL;
    bool cr0_changed=false, cr4_changed=false, vmx_active=false, svm_changed=false, hsave_changed=false;
    uint64_t old_cr0=read_cr0(),old_cr4=cm_read_cr4(),old_efer=0,old_hsave=0,phys=0;
    r.status=CM_VIRT_HW_FAULT;
    /* Verify both recovery vectors before attempting hardware enablement. */
    r.stage="exception recovery";
    uint64_t ignored;
    if (cm_hw_test_ud()!=-1 || fault_vector!=6 ||
        cm_hw_rdmsr(0xffffffffu,&ignored)!=-1 || fault_vector!=13) {
        r.status=CM_VIRT_HW_INVALID; goto cleanup;
    }
    r.recovery_tested=true; fault_vector=0;
    if (r.vmx) {
        uint64_t feature,basic,f0,f1;
        r.stage="IA32_FEATURE_CONTROL";
        if (cm_hw_rdmsr(0x3a,&feature)) goto cleanup;
        /* Unlocked MSR would require setting an irreversible lock; skip. */
        if ((feature&5)!=5 || (old_cr4&(1ull<<13))) { r.status=CM_VIRT_HW_POLICY; goto cleanup; }
        r.stage="VMX_BASIC/CR0 fixed bits";
        if (cm_hw_rdmsr(0x480,&basic) || cm_hw_rdmsr(0x486,&f0) || cm_hw_rdmsr(0x487,&f1)) goto cleanup;
        uint64_t target_cr0=old_cr0|f0;
        if ((target_cr0&~f1) || ((basic>>32)&0x1fff)>4096 ||
            ((basic>>32)&0x1fff)==0 || ((basic>>50)&15)!=6) { r.status=CM_VIRT_HW_INVALID; goto cleanup; }
        r.stage="CR4 fixed bits";
        if (cm_hw_rdmsr(0x488,&f0) || cm_hw_rdmsr(0x489,&f1)) goto cleanup;
        uint64_t target=old_cr4|(1ull<<13);
        /* Preserve existing CR4 features; only VMXE may be added. */
        if ((target&f0)!=f0 || (target&~f1)) { r.status=CM_VIRT_HW_INVALID; goto cleanup; }
        r.stage="region allocation/address";
        page=cm_page_alloc(0);
        if (!page) { r.status=CM_VIRT_HW_NOMEM; goto cleanup; }
        phys=(uint64_t)(uintptr_t)page-cm_hhdm();
        if ((phys&4095) || ((basic&(1ull<<48)) && phys>>32)) { r.status=CM_VIRT_HW_INVALID; goto cleanup; }
        memset(page,0,4096); *(uint32_t *)page=(uint32_t)basic&0x7fffffff;
        if (abort_prepare) { r.status=CM_VIRT_HW_ABORTED; goto cleanup; }
        r.stage="CR0/CR4 enable";
        if (target_cr0!=old_cr0) {
            if (cm_hw_cr0(target_cr0)) goto cleanup;
            cr0_changed=true;
        }
        if (cm_hw_cr4(target)) goto cleanup;
        cr4_changed=true;
        r.stage="VMXON";
        int code=cm_hw_vmxon(&phys);
        if (code) { r.instruction_status=(unsigned)code; r.status=code<0 ? CM_VIRT_HW_FAULT : CM_VIRT_HW_INSTRUCTION; goto cleanup; }
        vmx_active=true; r.initialized=true; r.status=CM_VIRT_HW_OK;
    } else {
        r.stage="SVM MSRs/firmware policy";
        uint64_t vmcr;
        if (cm_hw_rdmsr(0xc0010114,&vmcr) || cm_hw_rdmsr(0xc0000080,&old_efer) ||
            cm_hw_rdmsr(0xc0010117,&old_hsave)) goto cleanup;
        if ((vmcr&(1ull<<4)) || (old_efer&(1ull<<12))) { r.status=CM_VIRT_HW_POLICY; goto cleanup; }
        page=cm_page_alloc(0);
        if (!page) { r.status=CM_VIRT_HW_NOMEM; goto cleanup; }
        phys=(uint64_t)(uintptr_t)page-cm_hhdm();
        if (phys&4095) { r.status=CM_VIRT_HW_INVALID; goto cleanup; }
        memset(page,0,4096);
        if (abort_prepare) { r.status=CM_VIRT_HW_ABORTED; goto cleanup; }
        r.stage="SVM HSAVE/SVME enable";
        if (cm_hw_wrmsr(0xc0010117,phys)) goto cleanup;
        hsave_changed=true;
        if (cm_hw_wrmsr(0xc0000080,old_efer|(1ull<<12))) goto cleanup;
        svm_changed=true;
        uint64_t observed;
        if (cm_hw_rdmsr(0xc0000080,&observed)) goto cleanup;
        if (!(observed&(1ull<<12))) { r.status=CM_VIRT_HW_INSTRUCTION; goto cleanup; }
        r.initialized=true; r.status=CM_VIRT_HW_OK;
    }
cleanup:
    /* Never free a page still referenced by hardware. */
    if (vmx_active && cm_hw_vmxoff()) { r.rolled_back=false; r.stage="VMXOFF rollback"; }
    if (r.rolled_back && cr4_changed && cm_hw_cr4(old_cr4)) r.rolled_back=false;
    if (r.rolled_back && cr0_changed && cm_hw_cr0(old_cr0)) r.rolled_back=false;
    if (svm_changed && cm_hw_wrmsr(0xc0000080,old_efer)) r.rolled_back=false;
    if (hsave_changed && cm_hw_wrmsr(0xc0010117,old_hsave)) r.rolled_back=false;
    if (r.rolled_back && (read_cr0()!=old_cr0 || cm_read_cr4()!=old_cr4)) {
        r.rolled_back=false; r.stage="CR0/CR4 restore verification";
    }
    if (r.rolled_back && (svm_changed || hsave_changed)) {
        uint64_t efer,hsave;
        if (cm_hw_rdmsr(0xc0000080,&efer) || cm_hw_rdmsr(0xc0010117,&hsave) ||
            efer!=old_efer || hsave!=old_hsave) {
            r.rolled_back=false; r.stage="SVM restore verification";
        }
    }
    if (!r.rolled_back) r.status=CM_VIRT_HW_ROLLBACK;
    if (page && r.rolled_back) cm_page_free(page,0);
    r.fault_vector=fault_vector;
    armed=false;
    __asm__ volatile("lidt %0"::"m"(idt):"memory");
    cm_irq_restore(flags);
    return r;
#endif
}
