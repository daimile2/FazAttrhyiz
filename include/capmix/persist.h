#ifndef CAPMIX_PERSIST_H
#define CAPMIX_PERSIST_H
#include "types.h"
/* Boot selftest controller: persistent root -> scratch root -> persistent root. */
void cm_m3_prepare(cm_error_t mount_result);
int cm_m3_finish(void);
#endif
