#include "capmix/kernel.h"
#include "capmix/boot.h"

#ifdef CAPMIX_HOSTED
int main(void)
{
    cm_boot_cmdline_snapshot();
    capmix_init(NULL, 0);
    int rc = capmix_selftest();
    kinfo("FazAttrhyiz hosted phase-0..8 complete, rc=%d", rc);
    return rc == 0 ? 0 : 1;
}
#endif
