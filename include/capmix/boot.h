#ifndef CAPMIX_BOOT_H
#define CAPMIX_BOOT_H
#include "types.h"
void cm_boot_cmdline_snapshot(void);
int cm_boot_parse_selftest(const char *line);
int cm_boot_is_selftest(void);
int cm_boot_has_option(const char *option);
void cm_product_start(cm_error_t root_status);
void cm_product_record_root(cm_error_t root_status);
#endif
