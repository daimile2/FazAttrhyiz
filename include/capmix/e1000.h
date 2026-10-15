#ifndef CAPMIX_E1000_H
#define CAPMIX_E1000_H
#include "types.h"
cm_error_t cm_e1000_init(void);
int cm_e1000_available(void);
int cm_e1000_ifid(void);
int cm_e1000_irq_enabled(void);
uint64_t cm_e1000_irq_count(void);
void cm_e1000_irq_isr(void);
cm_error_t cm_e1000_transmit(const uint8_t *,uint16_t);
cm_error_t cm_e1000_receive(uint8_t *,uint16_t *);
int cm_m8_selftest(void);
#endif
