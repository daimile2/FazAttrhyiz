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
    uint8_t digest[CM_SHA256_SIZE];
    if (!s->expected_digest || s->digest_size != CM_SHA256_SIZE ||
        cm_sha256(s->content, s->content_size, digest) != CM_OK) {
        kinfo("sign: reject missing/invalid digest or content id=%s", s->id ? s->id : "-");
        return CM_ERR_DENIED;
    }
    /* Examine every digest byte without an early mismatch exit. */
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < CM_SHA256_SIZE; i++) diff |= digest[i] ^ s->expected_digest[i];
    if (diff) {
        kinfo("sign: digest mismatch rejected id=%s", s->id ? s->id : "-");
        return CM_ERR_DENIED;
    }
    kinfo("sign: digest verified id=%s flags=%x", s->id ? s->id : "-", s->flags);
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
