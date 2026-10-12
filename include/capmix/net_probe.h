#ifndef CAPMIX_NET_PROBE_H
#define CAPMIX_NET_PROBE_H
#include "types.h"
/* Bounded BSP selftest, independent state for each invocation. */
cm_error_t cm_net_probe(int ifid,const char *label,
    cm_error_t (*send)(const uint8_t *,uint16_t),
    cm_error_t (*recv)(uint8_t *,uint16_t *));
#endif
