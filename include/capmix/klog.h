#ifndef CAPMIX_KLOG_H
#define CAPMIX_KLOG_H

#include "types.h"

typedef enum {
    CM_LOG_TRACE = 0,
    CM_LOG_INFO  = 1,
    CM_LOG_WARN  = 2,
    CM_LOG_ERR   = 3,
    CM_LOG_PANIC = 4,
} cm_log_level_t;

void cm_klog_init(void);
void cm_klog(cm_log_level_t level, const char *fmt, ...);
void cm_puts(const char *s);
void cm_putchar(char c);

/* Returns 1 if acquired, 0 if the bounded spin gave up.
 * Callers MUST skip cm_klog_unlock() when they get 0 — see serial.c. */
int  cm_klog_lock(void);
void cm_klog_unlock(void);

#define ktrace(...) cm_klog(CM_LOG_TRACE, __VA_ARGS__)
#define kinfo(...)  cm_klog(CM_LOG_INFO,  __VA_ARGS__)
#define kwarn(...)  cm_klog(CM_LOG_WARN,  __VA_ARGS__)
#define kerr(...)   cm_klog(CM_LOG_ERR,   __VA_ARGS__)

void cm_panic(const char *fmt, ...);

#endif