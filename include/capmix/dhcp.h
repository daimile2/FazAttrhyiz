#ifndef CAPMIX_DHCP_H
#define CAPMIX_DHCP_H
#include "types.h"
int cm_dhcp_receiving(int ifid);
int cm_dhcp_offered_address(int ifid,uint32_t address);
cm_error_t cm_dhcp_input(int ifid,const uint8_t *data,uint16_t length);
/* Bootstrap-only, before user network applications; preserves config on failure. */
cm_error_t cm_dhcp_configure(int ifid,unsigned timeout_ms,int discard_replies);
void cm_dhcp_boot_configure(void);
int cm_dhcp_codec_selftest(void);
cm_error_t cm_net_dhcp_send(int ifid,uint8_t *frame,uint16_t length);
cm_error_t cm_net_boot_address(int ifid,uint32_t addr,uint32_t mask,uint32_t gateway);
#endif
