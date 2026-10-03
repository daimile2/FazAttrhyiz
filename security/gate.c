#include "capmix/security.h"
#include "capmix/klog.h"

extern void cm_seatbelt_init(void);
extern void cm_sip_init(void);
extern void cm_authz_init(void);
extern void cm_sign_init(void);

cm_error_t cm_security_init(void)
{
    cm_seatbelt_init();
    cm_sip_init();
    cm_authz_init();
    cm_sign_init();
    kinfo("sec: cap deny-by-default + Seatbelt + SIP + one-shot authz + sign");
    return CM_OK;
}

cm_error_t cm_cap_check(cm_task_t *task, int hid, cm_rights_t need)
{
    if (!need) return CM_ERR_DENIED; /* deny-by-default: asking nothing is nonsense */
    if (!task) return CM_ERR_DENIED;
    cm_handle_t *h = cm_handle_lookup(task, hid);
    if (!h || !h->obj) return CM_ERR_DENIED;
    if ((h->rights & need) != need) return CM_ERR_DENIED;
    return CM_OK;
}

cm_error_t cm_dac_check(const cm_dac_t *dac, uint32_t uid, uint32_t need_op)
{
    if (!dac) return CM_ERR_DENIED;
    if (dac->acl_deny & need_op) return CM_ERR_DENIED;
    if (dac->acl_allow & need_op) return CM_OK;
    /* Owner shortcut — still weaker than caps. */
    if (uid == dac->uid && (dac->mode & 0200) && (need_op & CM_OP_FILE_WRITE))
        return CM_OK;
    if (uid == dac->uid && (dac->mode & 0400) && (need_op & CM_OP_FILE_READ))
        return CM_OK;
    return CM_ERR_DENIED;
}

cm_error_t cm_sec_gate(cm_task_t *task, int hid, cm_rights_t cap_need,
                       cm_id_t pid, uint32_t op)
{
    if (cm_cap_check(task, hid, cap_need) != CM_OK) return CM_ERR_DENIED;
    if (cm_seatbelt_check(pid, op) != CM_OK) return CM_ERR_DENIED;
    if (cm_lsm_check(op, pid, 0) != CM_OK) return CM_ERR_DENIED;
    return CM_OK;
}
