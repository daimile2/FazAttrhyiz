#ifndef CAPMIX_FREESTANDING_STRING_H
#define CAPMIX_FREESTANDING_STRING_H

#include <stddef.h>

void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
