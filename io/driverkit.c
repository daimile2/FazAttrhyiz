#include "capmix/io.h"
#include "capmix/proc.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

extern void cm_blk_set_offline(int off);

static cm_driver_t g_drv[CM_MAX_DRIVERS];
static uint32_t    g_bound;

cm_error_t cm_drv_init(void)
{
    memset(g_drv, 0, sizeof(g_drv));
    g_bound = 0;
    kinfo("drv: user-mode framework (DriverKit/UMDF), kernel bus is thin");
    return CM_OK;
}

int cm_drv_kernel_alive(void)
{
    /* Probe: if the kernel heap is gone, this fails. Not a stored flag. */
    void *p = cm_page_alloc(0);
    if (!p) return 0;
    cm_page_free(p, 0);
    return 1;
}

cm_driver_t *cm_drv_get(int id)
{
    if (id < 0 || id >= CM_MAX_DRIVERS) return NULL;
    if (g_drv[id].state == CM_DRV_EMPTY) return NULL;
    return &g_drv[id];
}

static void drv_bottom(int irq, void *arg)
{
    int id = (int)(uintptr_t)arg;
    cm_driver_t *d = cm_drv_get(id);
    if (!d) return;
    d->irqs++;
    if (d->state != CM_DRV_ONLINE) {
        kinfo("drv: irq %d dropped, %s is offline (kernel continues)", irq, d->name);
        return;
    }
    /* Would IPC the IRQ packet to the user driver channel here.
     * Isolation: we never run driver code in this context. */
    d->ios++;
}

int cm_drv_bind(const char *name, int irq, cm_id_t pid)
{
    int id = -1;
    for (int i = 0; i < CM_MAX_DRIVERS; i++)
        if (g_drv[i].state == CM_DRV_EMPTY) { id = i; break; }
    if (id < 0) return -1;
    g_drv[id].id = id;
    g_drv[id].name = name ? name : "umd";
    g_drv[id].state = CM_DRV_ONLINE;
    g_drv[id].irq = irq;
    g_drv[id].pid = pid;
    g_bound++;
    cm_irq_request(irq, drv_bottom, (void *)(uintptr_t)id, name);
    kinfo("drv: bind %s id=%d irq=%d pid=%llu (user-mode)",
          g_drv[id].name, id, irq, (unsigned long)pid);
    return id;
}

cm_error_t cm_drv_crash(int id)
{
    cm_driver_t *d = cm_drv_get(id);
    if (!d) return CM_ERR_INVAL;
    d->state = CM_DRV_OFFLINE;
    d->crashes++;
    cm_blk_set_offline(1);
    cm_proc_t *p = cm_proc_lookup(d->pid);
    if (p && !p->exited) {
        p->exited = 1;
        p->exit_code = 11; /* SIGSEGV stand-in */
        if (p->main) p->main->state = CM_TH_DEAD;
        if (p->aspace_pages) {
            cm_commit_uncharge(p->aspace_pages);
            p->aspace_pages = 0;
        }
    }
    /* Isolation evidence is not this flag. Selftest must still allocate
     * and schedule after this returns. g_alive stays 1 only because we
     * did not call cm_panic. */
    kwarn("drv: %s task marked dead, device offline — prove kernel by later alloc",
          d->name);
    cm_obs_emit(cm_trace_new(), CM_EV_OOM, (uint64_t)id, d->crashes, 1);
    return CM_OK;
}

cm_error_t cm_io_init(void)
{
    cm_irq_init();
    cm_uring_init();
    cm_blk_init();
    cm_drv_init();
    return CM_OK;
}
