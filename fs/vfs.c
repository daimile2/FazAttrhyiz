#include "capmix/fs.h"
#include "capmix/klog.h"

extern cm_error_t cm_cow_vol_init(void);

cm_error_t cm_vfs_init(void)
{
    return cm_cow_vol_init();
}
