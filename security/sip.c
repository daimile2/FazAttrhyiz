#include "capmix/security.h"
#include "capmix/arch.h"
#include "capmix/klog.h"

static uint32_t g_sip = CM_SIP_DEFAULT;
static uint64_t g_slide = 0;
static int      g_mitigations = 0;

uint32_t cm_sip_mask(void) { return g_sip; }
uint64_t cm_kaslr_slide(void) { return g_slide; }
int      cm_mitigations(void) { return g_mitigations; }

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
    if (!(g_sip & CM_SIP_UNTRUSTED_KEXT)) return 1;
    if (!s) return 0;
    return (s->flags & (CM_SIGN_SIGNED | CM_SIGN_MODULE)) ==
           (CM_SIGN_SIGNED | CM_SIGN_MODULE);
}

void cm_sip_init(void)
{
    uint64_t cr4 = cm_read_cr4();
    g_mitigations = 0;
    if (cr4 & (1ull << 20)) g_mitigations |= 0x02; /* SMEP */
    if (cr4 & (1ull << 21)) g_mitigations |= 0x04; /* SMAP */
    /* KPTI/KASLR are policy bits we have not flipped in hardware. */
    kinfo("sip: mask=%x cr4=%llx mitigations=%x (only SMEP/SMAP bits that are actually set)",
          g_sip, (unsigned long)cr4, g_mitigations);
}
