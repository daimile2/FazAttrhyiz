#ifndef CAPMIX_KERNEL_H
#define CAPMIX_KERNEL_H

#include "types.h"
#include "config.h"
#include "klog.h"
#include "arch.h"
#include "obs.h"
#include "mm.h"
#include "cap.h"
#include "sched.h"
#include "ipc.h"
#include "syscall.h"
#include "proc.h"
#include "net.h"
#include "virt.h"

void capmix_init(const cm_mem_region_t *regions, int nregions);
void capmix_after_paging(void);
int  capmix_selftest(void);
void cm_selftest_finish(void);
void capmix_banner(void);

#endif
