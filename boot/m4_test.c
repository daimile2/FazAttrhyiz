#include "capmix/virtio_net.h"
#include "capmix/klog.h"
int cm_m4_selftest(void)
{
    int failed=cm_vnet_codec_selftest();
    if(!cm_virtio_net_available()) {
        kinfo("m4 virtio-net skipped: no supported NIC (codec failures=%d)",failed);return failed;
    }
    cm_error_t e=cm_vnet_probe();
    if(e){kerr("m4 virtio-net tx/rx failed: %d",e);failed++;}
    kinfo("m4 virtio-net selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);
    return failed;
}
