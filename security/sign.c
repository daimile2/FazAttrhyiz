#include "capmix/security.h"
#include "capmix/klog.h"

static cm_lsm_hook_t g_lsm;

cm_error_t cm_sign_verify(const cm_sign_t *s, int want_module)
{
    if (!s || s->flags == CM_SIGN_NONE) {
        kwarn("sign: unsigned image refused");
        return CM_ERR_DENIED;
    }
    if ((s->flags & CM_SIGN_SIGNED) == 0) return CM_ERR_DENIED;
    if (want_module && (s->flags & CM_SIGN_MODULE) == 0) return CM_ERR_DENIED;
    if (want_module && (cm_sip_mask() & CM_SIP_UNTRUSTED_KEXT) &&
        (s->flags & CM_SIGN_NOTARIZED) == 0) {
        kwarn("sign: module '%s' not notarized", s->id ? s->id : "?");
        return CM_ERR_DENIED;
    }
    kinfo("sign: ok id=%s flags=%x", s->id ? s->id : "-", s->flags);
    return CM_OK;
}

void cm_lsm_register(cm_lsm_hook_t h)
{
    g_lsm = h;
    kinfo("lsm: hook %s", h ? "installed (optional MAC)" : "cleared");
}

cm_error_t cm_lsm_check(uint32_t op, cm_id_t subject, uint64_t obj)
{
    if (!g_lsm) return CM_OK;
    int r = g_lsm(op, subject, obj);
    return r == 0 ? CM_OK : CM_ERR_DENIED;
}

void cm_sign_init(void)
{
    g_lsm = NULL;
}
