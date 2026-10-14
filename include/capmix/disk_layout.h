#ifndef CAPMIX_DISK_LAYOUT_H
#define CAPMIX_DISK_LAYOUT_H
#include "types.h"
#define CM_INSTALL_DISK_ID 0x43504d58u
#define CM_INSTALL_ROOT_LBA 2048u
#define CM_INSTALL_ROOT_SECT 16384u
#define CM_INSTALL_ESP_LBA 18432u
#define CM_INSTALL_ESP_SECT 131072u
#define CM_INSTALL_MIN_SECT 262144u
/* NOENT means legacy/unmarked; INVAL means marked but unsafe (never fall back). */
cm_error_t cm_disk_layout_parse(const void *sector,uint64_t capacity,uint64_t *base);
uint64_t cm_blk_root_base(void);
int cm_blk_installed(void);
int cm_disk_layout_selftest(void);
#endif
