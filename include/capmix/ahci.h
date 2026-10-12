#ifndef CAPMIX_AHCI_H
#define CAPMIX_AHCI_H
#include "types.h"
cm_error_t cm_ahci_init(void);
int cm_ahci_available(void);
cm_error_t cm_ahci_rw(uint32_t op,uint64_t lba,uint32_t count,void *buffer);
cm_error_t cm_ahci_flush(void);
int cm_ahci_selftest(void);
#endif
