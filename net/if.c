#include "capmix/net.h"
#include "capmix/klog.h"

#include <string.h>

static cm_iface_t g_if[CM_MAX_IFACE];
static int        g_nif;

cm_error_t cm_net_core_if_init(void)
{
    memset(g_if, 0, sizeof(g_if));
    g_nif = 0;
    return CM_OK;
}

int cm_if_add(const char *name, uint32_t addr)
{
    if (g_nif >= CM_MAX_IFACE) return -1;
    int id = g_nif++;
    g_if[id].up = 1;
    g_if[id].name = name;
    g_if[id].addr = addr;
    g_if[id].xdp_prog = -1;
    kinfo("net: iface %s id=%d addr=%x", name, id, addr);
    return id;
}

cm_iface_t *cm_if_get(int id)
{
    if (id < 0 || id >= g_nif) return NULL;
    return &g_if[id];
}

int cm_if_find(const char *name)
{
    if (!name) return -1;
    size_t len = strlen(name);
    for (int i = 0; i < g_nif; i++)
        if (g_if[i].name && strlen(g_if[i].name) == len && !memcmp(g_if[i].name, name, len)) return i;
    return -1;
}

void cm_net_dump_if(void)
{
    for (int i = 0; i < g_nif; i++)
        kinfo("  %s up=%d rx=%llu tx=%llu drop=%llu xdp=%d",
              g_if[i].name, g_if[i].up,
              (unsigned long)g_if[i].rx, (unsigned long)g_if[i].tx,
              (unsigned long)g_if[i].drop, g_if[i].xdp_prog);
}

cm_iface_t *cm_net_primary_iface(void)
{
    cm_iface_t *v=cm_if_get(cm_if_find("vnet0"));
    return v?v:cm_if_get(cm_if_find("enet0"));
}
