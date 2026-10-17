/* Link state and fallback use the production selector, without PCI mocks. */
#include "capmix/net.h"
#include "capmix/klog.h"
#include <assert.h>
#include <stdio.h>
void cm_klog(cm_log_level_t level,const char *fmt,...) {(void)level;(void)fmt;}
extern cm_error_t cm_net_core_if_init(void);
int main(void)
{
    assert(cm_net_core_if_init()==CM_OK);
    assert(!cm_net_primary_iface());
    int e=cm_if_add("enet0",0x0a00020f),v=cm_if_add("vnet0",0x0a00020f);
    cm_iface_t *enet=cm_if_get(e),*vnet=cm_if_get(v);
    assert(enet && vnet && cm_net_primary_iface()==vnet);
    vnet->up=0;assert(cm_net_primary_iface()==enet);
    enet->up=0;assert(!cm_net_primary_iface());
    vnet->up=1;assert(cm_net_primary_iface()==vnet);
    enet->up=1;assert(cm_net_primary_iface()==vnet);
    puts("net primary verified: absent/down=none up-recovery=ok priority=virtio>e1000 fallback=ok");
    return 0;
}
