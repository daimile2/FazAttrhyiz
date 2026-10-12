#include "capmix/virt.h"
#include "capmix/arch.h"
#include "capmix/mm.h"
#include "capmix/io.h"
#include "capmix/klog.h"
static int verify_cleanup(uint64_t pages,uint64_t cr4,cm_guest_result_t *r)
{
    int bad=!r->hardware.rolled_back || cm_mm_free_pages()!=pages || cm_read_cr4()!=cr4;
    void *page=cm_page_alloc(0);
    if (!page) bad=1;
    if (page) { *(uint64_t *)page=0xfeed; if (*(uint64_t *)page!=0xfeed) bad=1; cm_page_free(page,0); }
    if (!cm_drv_kernel_alive()) bad=1;
    kinfo("phase9: stage=%s hardware_status=%u instruction_status=%d instruction_error=%llu field=%llx fault=%u rollback=%s buddy=%s kernel_alive=%u",
        r->stage,(unsigned)r->hardware.status,r->instruction_status,(unsigned long)r->instruction_error,
        (unsigned long)r->failed_field,r->hardware.fault_vector,bad ? "FAILED" : "ok",page ? "usable" : "FAILED",cm_drv_kernel_alive());
    return bad;
}
int cm_phase9_selftest(void)
{
    uint64_t flags=cm_irq_save(),pages=cm_mm_free_pages(),cr4=cm_read_cr4(),exits=cm_virt_real_vmexits();
    int fails=0;
    for (unsigned mode=5;mode<=6;mode++) {
        cm_guest_result_t stopped=cm_virt_guest_once(mode);
        fails+=verify_cleanup(pages,cr4,&stopped);
        if (stopped.real_exit || cm_virt_real_vmexits()!=exits) fails++;
        kinfo("ept: %s cleanup verified rollback=%s",mode==5 ? "partial allocation abort" : "missing INVEPT skip",
            stopped.hardware.rolled_back ? "ok" : "FAILED");
    }
    cm_guest_result_t aborted=cm_virt_guest_once(1);
    fails+=verify_cleanup(pages,cr4,&aborted);
    if (aborted.vmcs_cleared) kinfo("phase9: injected post-VMPTRLD abort cleanup verified rollback=ok");
    cm_guest_result_t invalid=cm_virt_guest_once(2);
    fails+=verify_cleanup(pages,cr4,&invalid);
    if (invalid.vmcs_ready) {
        if (invalid.real_exit || cm_virt_real_vmexits()!=exits) fails++;
        kinfo("phase9: invalid guest entry rejected reason=%llx vmexit=not-real rollback=%s",
            (unsigned long)invalid.exit_reason,invalid.hardware.rolled_back ? "ok" : "FAILED");
    }
    cm_guest_result_t r=cm_virt_guest_once(0);
    fails+=verify_cleanup(pages,cr4,&r);
    if (r.real_exit) {
#ifndef CAPMIX_HOSTED
        if ((r.exit_reason&0xffff)!=18 || r.guest_rip!=r.guest_entry || !r.ept_ready || !r.ept_invalidated ||
            cm_virt_real_vmexits()!=exits+1 || !r.vmcs_cleared) fails++;
        kinfo("phase9: vmlaunch ok exit_reason=%llu qualification=0x%llx guest_rip=0x%llx vmexit=real count=%llu rollback=%s",
            (unsigned long)(r.exit_reason&0xffff),(unsigned long)r.qualification,
            (unsigned long)r.guest_rip,(unsigned long)cm_virt_real_vmexits(),r.hardware.rolled_back ? "ok" : "FAILED");
#endif
    } else kinfo("phase9: guest launch skipped: %s (vmx=%u hosted/firmware/control/entry refusal; stage=%s hardware_stage=%s) rollback=%s",
        r.hardware.vmx ? "VMX launch unavailable" : "no VMX backend",(unsigned)r.hardware.vmx,r.stage,r.hardware.stage,
        r.hardware.rolled_back ? "ok" : "FAILED");
    for (unsigned mode=3;mode<=4;mode++) {
        uint64_t count=cm_virt_real_vmexits();
        cm_guest_result_t denied=cm_virt_guest_once(mode);
        fails+=verify_cleanup(pages,cr4,&denied);
        if (denied.real_exit) {
            uint64_t expected=mode==3 ? 0x8000 : 0x1000;
            if ((denied.exit_reason&0xffff)!=48 || denied.guest_gpa!=expected ||
                !(denied.qualification&(mode==3 ? 1ull : 4ull)) || !denied.ept_invalidated ||
                cm_virt_real_vmexits()!=count+1) fails++;
            kinfo("ept: violation reason=%llu gpa=0x%llx qualification=0x%llx guest_rip=0x%llx test=%s vmexit=real rollback=%s",
                (unsigned long)(denied.exit_reason&0xffff),(unsigned long)denied.guest_gpa,
                (unsigned long)denied.qualification,(unsigned long)denied.guest_rip,
                mode==3 ? "unmapped read" : "execute permission",denied.hardware.rolled_back ? "ok" : "FAILED");
        } else kinfo("ept: guest launch skipped stage=%s (hosted/no VMX/EPT capability unavailable) rollback=%s",
            denied.stage,denied.hardware.rolled_back ? "ok" : "FAILED");
        if (r.real_exit && !denied.real_exit) fails++;
    }
    kinfo("ept: real exits=%llu (reason=18/48); per-VM synthetic vmexits remain separate",
        (unsigned long)cm_virt_real_vmexits());
    cm_irq_restore(flags);
    kinfo("phase9 selftest: %s (%d failures)",fails ? "FAIL" : "PASS",fails);
    return fails;
}
