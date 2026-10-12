#ifndef CAPMIX_VIRTIO_NET_H
#define CAPMIX_VIRTIO_NET_H
#include "types.h"
cm_error_t cm_virtio_net_init(void);
int cm_virtio_net_available(void);
int cm_virtio_net_ifid(void);
cm_error_t cm_virtio_net_transmit(const uint8_t *frame,uint16_t length);
cm_error_t cm_virtio_net_receive(uint8_t *frame,uint16_t *length);
int cm_m4_selftest(void);
cm_error_t cm_vnet_probe(void);
int cm_vnet_codec_selftest(void);
#endif
