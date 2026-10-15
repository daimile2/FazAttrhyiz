#ifndef CAPMIX_ABI_H
#define CAPMIX_ABI_H
#include "types.h"
typedef enum {CM_ABI_CAPMIX=0,CM_ABI_LINUX=1} cm_abi_t;
struct cm_proc;
int cm_linux_enabled(void);
void cm_linux_random16(uint8_t out[16]);
int64_t cm_linux_dispatch(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t);
/* Internal bridges, never recursively dispatch Linux numbers as native ones. */
int64_t cm_native_write(uint64_t,uint64_t,uint64_t);
int64_t cm_native_exit(uint64_t);
uint64_t cm_arch_user_fs_get(void);
void cm_arch_user_fs_set(uint64_t);
int cm_linux_selftest(void);
#endif
