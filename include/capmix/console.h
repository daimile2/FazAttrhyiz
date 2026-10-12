#ifndef CAPMIX_CONSOLE_H
#define CAPMIX_CONSOLE_H
#include "types.h"
typedef struct {
    uint64_t physical, width, height, pitch;
    uint16_t bpp;
    uint8_t model, red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
} cm_fb_info_t;
void cm_limine_fb_snapshot(void);
int cm_limine_fb_info(cm_fb_info_t *out);
void cm_console_init(void);
void cm_console_putchar(char c);
void cm_console_poll(void);
void cm_console_idle(void);
int cm_console_selftest(void);
int cm_ps2_init(void);
int cm_ps2_selftest(void);
void cm_ps2_irq(void);
void cm_ps2_poll(void);
/* Product shell consumes decoded characters; old echo polling stays unchanged. */
cm_error_t cm_ps2_read_char(char *out);
#endif
