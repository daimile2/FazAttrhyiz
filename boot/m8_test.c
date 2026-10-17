#include "capmix/e1000.h"
#include "capmix/net_probe.h"
#include "capmix/net.h"
#include "capmix/klog.h"
int cm_m8_selftest(void)
{
    if(!cm_e1000_available()){kinfo("m8 e1000 skipped: no supported NIC");return 0;}
    cm_error_t ready=cm_e1000_wait_link(6000);
    if(ready==CM_ERR_AGAIN || ready==CM_ERR_NOTSUP) {
        kinfo("m8 e1000 wire SKIP: link readiness=%d; carrier unavailable or uncalibrated clock, no tx/rx claim",ready);
        return 0;
    }
    if(ready){kerr("m8 e1000 link check failed: %d",ready);return 1;}
    cm_iface_t *iface=cm_if_get(cm_e1000_ifid());
    uint64_t tx=iface->tx,rx=iface->rx;cm_error_t e=CM_OK;
    /* 18 TX/RX packets cross the 16-entry rings, verifying descriptor reuse. */
    for(unsigned i=0;i<9 && !e;i++)
        e=cm_net_probe(cm_e1000_ifid(),"e1000",cm_e1000_transmit,cm_e1000_receive);
    if(!e && (iface->tx-tx<18 || iface->rx-rx<18))e=CM_ERR_IO;
    if(!e)kinfo("m8 e1000 ring wrap verified: probes=9 tx_delta=%llu rx_delta=%llu",
                (unsigned long)(iface->tx-tx),(unsigned long)(iface->rx-rx));
    if(e)kerr("m8 e1000 tx/rx failed: %d",e);
    kinfo("m8 e1000 selftest: %s (%d failures)",e?"FAIL":"PASS",!!e);
    return !!e;
}
