#include "capmix/security.h"
#include "capmix/arch.h"
#include "capmix/config.h"
#include "capmix/klog.h"

static uint32_t g_sip = CM_SIP_DEFAULT;
static uint64_t g_slide = 0;


uint32_t cm_sip_mask(void) { return g_sip; }
uint64_t cm_kaslr_slide(void) { return g_slide; }
int cm_mitigations(void)
{
    uint64_t cr4=cm_read_cr4();
    return ((cr4&(1ull<<20)) ? CM_MIT_SMEP : 0) |
           ((cr4&(1ull<<21)) ? CM_MIT_SMAP : 0) |
           (cm_kpti_active() ? CM_MIT_KPTI : 0);
}
void cm_mitigations_dump(void)
{
    int bits=cm_mitigations();
    kinfo("mitigations=%x SMEP=%u SMAP=%u KPTI=%u kpti_configured=%u kaslr=unimplemented kaslr_slide=0",
          bits,!!(bits&CM_MIT_SMEP),!!(bits&CM_MIT_SMAP),!!(bits&CM_MIT_KPTI),(unsigned)CM_KPTI);
    if (CM_KPTI && !(bits&CM_MIT_KPTI))
        kinfo("mitigations: KPTI awaiting actual ring3 shadow/full CR3 verification (hosted has no hardware entry)");
}

bool cm_sip_protects(cm_vaddr_t addr)
{
    if (!(g_sip & CM_SIP_KERN_TEXT)) return false;
    return addr >= (CM_KTEXT_BASE + g_slide) || addr >= CM_KTEXT_BASE;
}

cm_error_t cm_sip_write_check(cm_vaddr_t addr)
{
    if (cm_sip_protects(addr)) {
        kwarn("sip: blocked write to %llx even for root", (unsigned long)addr);
        return CM_ERR_DENIED;
    }
    if ((g_sip & CM_SIP_SSV) && addr >= 0xffffff8000000000ull) {
        kwarn("sip: SSV sealed");
        return CM_ERR_DENIED;
    }
    return CM_OK;
}

int cm_sip_allows_kext(const cm_sign_t *s)
{
    return cm_sign_verify(s, 1) == CM_OK;
}

void cm_sip_init(void)
{
    kinfo("sip: mask=%x cr4=%llx",g_sip,(unsigned long)cm_read_cr4());
    cm_mitigations_dump();
}
