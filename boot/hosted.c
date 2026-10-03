#include "capmix/kernel.h"

#ifdef CAPMIX_HOSTED
int main(void)
{
    capmix_init(NULL, 0);
    int rc = capmix_selftest();
    kinfo("CapMix hosted phase-0..8 complete, rc=%d", rc);
    return rc == 0 ? 0 : 1;
}
#endif
