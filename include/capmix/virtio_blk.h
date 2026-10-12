#ifndef CAPMIX_VIRTIO_BLK_H
#define CAPMIX_VIRTIO_BLK_H
#include "types.h"
cm_error_t cm_virtio_blk_init(void);
int cm_virtio_blk_available(void);
uint64_t cm_virtio_blk_capacity(void);
cm_error_t cm_virtio_blk_rw(uint32_t op,uint64_t sector,uint32_t count,void *buffer);
cm_error_t cm_virtio_blk_flush(void);
uint64_t cm_virtio_blk_requests(void);
uint64_t cm_virtio_blk_flushes(void);
#endif
