#include "capmix/io.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include "capmix/obs.h"
#include <string.h>

typedef struct {
    cm_irq_handler_t h;
    void *arg;
    const char *name;
    uint64_t hits, generation;
} cm_irq_line_t;
typedef struct { int irq; void *arg; cm_irq_handler_t h; uint64_t generation; } cm_soft_item_t;
static cm_irq_line_t g_line[CM_MAX_IRQ];
static cm_soft_item_t g_sq[CM_SOFTIRQ_MAX];
static uint32_t g_sq_h, g_sq_t, g_sq_n;
static uint64_t g_top, g_bot, g_overflow, g_stale;
static volatile unsigned g_irq_lock;
static uint64_t irq_lock(void)
{
    uint64_t flags = cm_irq_save();
    while (__sync_lock_test_and_set(&g_irq_lock, 1u)) __asm__ volatile("pause" ::: "memory");
    return flags;
}
static void irq_unlock(uint64_t flags)
{ __sync_lock_release(&g_irq_lock); cm_irq_restore(flags); }
cm_error_t cm_irq_init(void)
{
    memset(g_line, 0, sizeof(g_line));
    g_sq_h = g_sq_t = g_sq_n = 0;
    g_top = g_bot = g_overflow = g_stale = 0;
    kinfo("irq: top/bottom half ready, %d lines", CM_MAX_IRQ);
    (void)cm_ioapic_init();
    return CM_OK;
}
cm_error_t cm_irq_request(int irq, cm_irq_handler_t h, void *arg, const char *name)
{
    if (irq < 0 || irq >= CM_MAX_IRQ || !h) return CM_ERR_INVAL;
    uint64_t flags = irq_lock();
    if (g_line[irq].h) { irq_unlock(flags); return CM_ERR_BUSY; }
    g_line[irq].h = h; g_line[irq].arg = arg; g_line[irq].name = name;
    g_line[irq].generation++;
    irq_unlock(flags);
    return CM_OK;
}
cm_error_t cm_irq_release(int irq, cm_irq_handler_t h, void *arg)
{
    if (irq < 0 || irq >= CM_MAX_IRQ) return CM_ERR_INVAL;
    uint64_t flags = irq_lock();
    if (g_line[irq].h != h || g_line[irq].arg != arg) { irq_unlock(flags); return CM_ERR_INVAL; }
    g_line[irq].h = NULL; g_line[irq].arg = NULL; g_line[irq].name = NULL;
    g_line[irq].generation++;
    irq_unlock(flags);
    return CM_OK;
}
void cm_irq_inject(int irq)
{
    /* Fixed work only: no allocation, tracing, logging, IPC, or scheduling. */
    uint64_t flags = irq_lock();
    g_top++;
    if (irq < 0 || irq >= CM_MAX_IRQ) { irq_unlock(flags); return; }
    cm_irq_line_t *l = &g_line[irq];
    l->hits++;
    if (!l->h) { irq_unlock(flags); return; }
    if (g_sq_n == CM_SOFTIRQ_MAX) { g_overflow++; irq_unlock(flags); return; }
    g_sq[g_sq_t] = (cm_soft_item_t){irq, l->arg, l->h, l->generation};
    g_sq_t = (g_sq_t + 1) % CM_SOFTIRQ_MAX; g_sq_n++;
    irq_unlock(flags);
}
uint64_t cm_irq_softirq_flush(void)
{
    /* A finite snapshot budget; callbacks and observations run outside lock. */
    uint64_t flags = irq_lock();
    unsigned budget = g_sq_n;
    irq_unlock(flags);
    uint64_t n = 0;
    while (budget--) {
        flags = irq_lock();
        if (!g_sq_n) { irq_unlock(flags); break; }
        cm_soft_item_t it = g_sq[g_sq_h];
        g_sq_h = (g_sq_h + 1) % CM_SOFTIRQ_MAX; g_sq_n--; g_bot++; n++;
        int valid = g_line[it.irq].generation == it.generation && g_line[it.irq].h == it.h;
        if (!valid) g_stale++;
        irq_unlock(flags);
        if (valid) {
            if (cm_obs_enabled()) cm_obs_emit(cm_trace_new(), CM_EV_IPC_SEND, it.irq, it.generation, 0);
            it.h(it.irq, it.arg);
        }
    }
    (void)cm_drv_irq_retry();
    return n;
}
uint64_t cm_irq_top_count(void)
{ uint64_t f = irq_lock(), n = g_top; irq_unlock(f); return n; }
uint64_t cm_irq_bot_count(void)
{ uint64_t f = irq_lock(), n = g_bot; irq_unlock(f); return n; }
uint64_t cm_irq_overflow_count(void)
{ uint64_t f = irq_lock(), n = g_overflow; irq_unlock(f); return n; }
uint64_t cm_irq_stale_count(void)
{ uint64_t f = irq_lock(), n = g_stale; irq_unlock(f); return n; }
