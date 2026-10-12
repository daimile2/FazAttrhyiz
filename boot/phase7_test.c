#include "capmix/virt_hw.h"
#include "capmix/arch.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
static const char *status_name(cm_virt_hw_status_t s)
{
    switch (s) {
    case CM_VIRT_HW_OK: return "initialized";
    case CM_VIRT_HW_UNSUPPORTED: return "no VMX/SVM";
    case CM_VIRT_HW_HOSTED: return "hosted privilege unavailable";
    case CM_VIRT_HW_POLICY: return "firmware disabled/unlocked or hardware already owned";
    case CM_VIRT_HW_INVALID: return "unsupported controls/region or recovery check failed";
    case CM_VIRT_HW_NOMEM: return "allocation failed";
    case CM_VIRT_HW_FAULT: return "MSR/instruction fault";
    case CM_VIRT_HW_INSTRUCTION: return "hardware instruction refused";
    case CM_VIRT_HW_ABORTED: return "injected prepare abort";
    case CM_VIRT_HW_ROLLBACK: return "ROLLBACK FAILED";
    }
    return "unknown";
}
int cm_phase7_selftest(void)
{
    int fails=0;
    uint64_t flags=cm_irq_save(), before=cm_mm_free_pages(), cr4=cm_read_cr4();
    cm_virt_hw_result_t aborted=cm_virt_hw_probe(true);
    if (!aborted.rolled_back || cm_mm_free_pages()!=before || cm_read_cr4()!=cr4) fails++;
    if (aborted.status==CM_VIRT_HW_ABORTED)
        kinfo("phase7: prepare abort rollback verified free_pages=%llu",(unsigned long)before);
    cm_virt_hw_result_t r=cm_virt_hw_probe(false);
    if (!r.rolled_back || cm_mm_free_pages()!=before || cm_read_cr4()!=cr4 ||
        (r.status==CM_VIRT_HW_OK && !r.initialized)) fails++;
    /* A failed deterministic recovery check is a software failure, not an
     * unsupported hardware skip. Capability restrictions are otherwise benign. */
#ifndef CAPMIX_HOSTED
    if ((r.vmx || r.svm) && !r.recovery_tested) fails++;
#endif
    cm_irq_restore(flags);
    kinfo("phase7: hardware detect vmx=%u svm=%u recovery_tested=%u",
          (unsigned)r.vmx,(unsigned)r.svm,(unsigned)r.recovery_tested);
    kinfo("phase7: %s status=%u reason=%s stage=%s fault_vector=%u instruction_status=%x rollback=%s",
          r.vmx ? "vmx" : r.svm ? "svm" : "virt",(unsigned)r.status,status_name(r.status),r.stage,
          r.fault_vector,r.instruction_status,r.rolled_back ? "ok" : "FAILED");
    if (r.initialized)
        kinfo("phase7: %s minimum init ok; state restored, buddy pages recovered",
              r.vmx ? "VMXON/VMXOFF" : "SVM EFER.SVME/VM_HSAVE_PA");
    else kinfo("phase7: hardware init skipped: %s",status_name(r.status));
    kinfo("phase7: guest launch skipped: probe-only test; phase9 owns single VMLAUNCH; existing per-VM counters synthetic");
    kinfo("phase7 selftest: %s (%d failures)",fails ? "FAIL" : "PASS",fails);
    return fails;
}
