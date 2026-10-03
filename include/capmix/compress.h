#ifndef CAPMIX_COMPRESS_H
#define CAPMIX_COMPRESS_H

#include "types.h"

/* WKdm-lite: 32-bit word dictionary + zero run. Inspects every word.
 * Not LZ4/zstd — those come later. This one is real: in != out, and
 * decompress(compress(p)) == p is tested.
 */

#define CM_WKDM_DICT 16
#define CM_WKDM_MAX  (CM_PAGE_SIZE + 256)

size_t cm_wkdm_compress(const void *src, size_t slen, void *dst, size_t dlen);
size_t cm_wkdm_decompress(const void *src, size_t slen, void *dst, size_t dlen);
int    cm_wkdm_selftest(void);

#endif
