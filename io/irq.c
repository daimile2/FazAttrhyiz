#include "capmix/io.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

typedef struct {
    cm_irq_handler_t h;
    void            *arg;
    const char      *name;
    uint64_t         hits;
} cm_irq_line_t;

typedef struct {
    int   irq;
    void *arg;
} cm_soft_item_t;

static cm_irq_line_t  g_line[CM_MAX_IRQ];
static cm_soft_item_t g_sq[CM_SOFTIRQ_MAX];
static uint32_t       g_sq_h, g_sq_t, g_sq_n;
static uint64_t       g_top, g_bot;

cm_error_t cm_irq_init(void)
{
    memset(g_line, 0, sizeof(g_line));
    g_sq_h = g_sq_t = g_sq_n = 0;
    g_top = g_bot = 0;
    kinfo("irq: top/bottom half ready, %d lines", CM_MAX_IRQ);

    /* Arch IRQ routing (IOAPIC on x86_64). Needs HHDM + live CR3 + mapped
     * LAPIC, all of which are up by the time irq init runs (see boot log:
     * "lapic: mapped" and "paging: CR3=" both precede this line).
     * Hosted, or bare metal with no RSDP: warns once and returns -1. */
    (void)cm_ioapic_init();

    return CM_OK;
}

cm_error_t cm_irq_request(int irq, cm_irq_handler_t h, void *arg, const char *name)
{
    if (irq < 0 || irq >= CM_MAX_IRQ || !h) return CM_ERR_INVAL;
    g_line[irq].h = h;
    g_line[irq].arg = arg;
    g_line[irq].name = name;
    return CM_OK;
}

void cm_irq_inject(int irq)
{
    /* Top half: no allocation, no locks held long, just queue work. */
    g_top++;
    if (irq < 0 || irq >= CM_MAX_IRQ) return;
    g_line[irq].hits++;
    if (g_sq_n >= CM_SOFTIRQ_MAX) return;
    g_sq[g_sq_t].irq = irq;
    g_sq[g_sq_t].arg = g_line[irq].arg;
    g_sq_t = (g_sq_t + 1) % CM_SOFTIRQ_MAX;
    g_sq_n++;
    cm_obs_emit(cm_trace_new(), CM_EV_ALLOC, (uint64_t)irq, g_top, 0);
}

uint64_t cm_irq_softirq_flush(void)
{
    uint64_t n = 0;
    while (g_sq_n) {
        cm_soft_item_t it = g_sq[g_sq_h];
        g_sq_h = (g_sq_h + 1) % CM_SOFTIRQ_MAX;
        g_sq_n--;
        g_bot++;
        n++;
        if (it.irq >= 0 && it.irq < CM_MAX_IRQ && g_line[it.irq].h)
            g_line[it.irq].h(it.irq, it.arg);
    }
    return n;
}

uint64_t cm_irq_top_count(void) { return g_top; }
uint64_t cm_irq_bot_count(void) { return g_bot; }
