#include "capmix/kernel.h"
#include "capmix/net.h"
#include "capmix/dhcp.h"
#include "capmix/boot.h"
int cm_p1c_selftest(void)
{
    int failed=cm_dhcp_codec_selftest(),any=0;
    for(int id=0;id<CM_MAX_IFACE;id++) {
        cm_iface_t *iface=cm_if_get(id);
        if(iface && iface->up && (id==cm_if_find("vnet0") || id==cm_if_find("enet0")))any=1;
    }
    if(!any){kinfo("dhcp no NIC skipped; p1c selftest: %s",failed?"FAIL":"PASS");return failed;}
    cm_error_t e=cm_net_bg_start();if(e)return failed+1;
    /* Real packet loss exercises the finite timeout without changing config. */
    for(int id=0;!cm_boot_has_option("capmix.net=static") && id<CM_MAX_IFACE;id++) {
        cm_iface_t *iface=cm_if_get(id);
        if(!iface || !iface->up || (id!=cm_if_find("vnet0") && id!=cm_if_find("enet0")))continue;
        uint32_t addr=iface->addr,mask=iface->netmask,gateway=iface->gateway;
        e=cm_dhcp_configure(id,50,1);
        if(e!=CM_ERR_AGAIN || addr!=iface->addr || mask!=iface->netmask || gateway!=iface->gateway)failed++;
        else kinfo("dhcp %s timeout fallback verified: original static address/mask/gateway intact",iface->name);
    }
    cm_dhcp_boot_configure();
    cm_net_bg_stop();
    kinfo("p1c selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
