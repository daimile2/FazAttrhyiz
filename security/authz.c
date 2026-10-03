#include "capmix/security.h"
#include "capmix/klog.h"

#include <string.h>

#define CM_AUTHZ_MAX 16

static struct {
    int used;
    cm_id_t pid;
    uint32_t op;
} g_tok[CM_AUTHZ_MAX];

int cm_authz_grant(cm_id_t pid, uint32_t op)
{
    for (int i = 0; i < CM_AUTHZ_MAX; i++) {
        if (g_tok[i].used) continue;
        g_tok[i].used = 1;
        g_tok[i].pid = pid;
        g_tok[i].op = op;
        kinfo("authz: one-shot token pid=%llu op=%x (not a whole-process token)",
              (unsigned long)pid, op);
        return i;
    }
    return -1;
}

cm_error_t cm_authz_consume(cm_id_t pid, uint32_t op)
{
    for (int i = 0; i < CM_AUTHZ_MAX; i++) {
        if (!g_tok[i].used || g_tok[i].pid != pid || g_tok[i].op != op)
            continue;
        g_tok[i].used = 0;
        kinfo("authz: consumed one-shot op=%x pid=%llu", op, (unsigned long)pid);
        return CM_OK;
    }
    return CM_ERR_DENIED;
}

void cm_authz_init(void)
{
    memset(g_tok, 0, sizeof(g_tok));
}
