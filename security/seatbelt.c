#include "capmix/security.h"
#include "capmix/klog.h"

#include <string.h>

#define CM_SB_MAX 32

static struct {
    int used;
    cm_id_t pid;
    cm_seatbelt_t p;
} g_sb[CM_SB_MAX];

cm_error_t cm_seatbelt_apply(cm_id_t pid, const cm_seatbelt_t *p)
{
    if (!p) return CM_ERR_INVAL;
    int slot = -1;
    for (int i = 0; i < CM_SB_MAX; i++) {
        if (g_sb[i].used && g_sb[i].pid == pid) { slot = i; break; }
        if (!g_sb[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return CM_ERR_NOSPC;
    g_sb[slot].used = 1;
    g_sb[slot].pid = pid;
    g_sb[slot].p = *p;
    kinfo("seatbelt: pid=%llu profile='%s' allow=%x deny=%x",
          (unsigned long)pid, p->label ? p->label : "-",
          p->allow_mask, p->deny_mask);
    return CM_OK;
}

cm_error_t cm_seatbelt_check(cm_id_t pid, uint32_t op)
{
    for (int i = 0; i < CM_SB_MAX; i++) {
        if (!g_sb[i].used || g_sb[i].pid != pid) continue;
        if (g_sb[i].p.deny_mask & op) return CM_ERR_DENIED;
        /* Per-operation: not listed in allow → deny. Empty allow = closed. */
        if ((g_sb[i].p.allow_mask & op) == 0) return CM_ERR_DENIED;
        return CM_OK;
    }
    /* No profile = unrestricted at this layer (caps / SIP still apply). */
    return CM_OK;
}

void cm_seatbelt_init(void)
{
    memset(g_sb, 0, sizeof(g_sb));
}
