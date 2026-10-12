#ifndef CAPMIX_UACCESS_H
#define CAPMIX_UACCESS_H
#include "capmix/elf.h"
#define CM_USER_IO_MAX (1024ull * 1024)
int cm_user_access(const cm_elf_image_t *, uint64_t, uint64_t, int);
cm_error_t cm_copyin(const cm_elf_image_t *, void *, uint64_t, uint64_t);
cm_error_t cm_copyout(const cm_elf_image_t *, uint64_t, const void *, uint64_t);
cm_error_t cm_copyinstr(const cm_elf_image_t *, char *, uint64_t, size_t);
#endif
