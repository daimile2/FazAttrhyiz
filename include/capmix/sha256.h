#ifndef CAPMIX_SHA256_H
#define CAPMIX_SHA256_H
#include "capmix/types.h"
#define CM_SHA256_SIZE 32
/* Initialize before use. Input/output must not overlap the context. */
typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
    bool finalized;
} cm_sha256_t;
cm_error_t cm_sha256_init(cm_sha256_t *ctx);
cm_error_t cm_sha256_update(cm_sha256_t *ctx, const void *data, size_t len);
cm_error_t cm_sha256_final(cm_sha256_t *ctx, uint8_t out[CM_SHA256_SIZE]);
cm_error_t cm_sha256(const void *data, size_t len, uint8_t out[CM_SHA256_SIZE]);
#endif
