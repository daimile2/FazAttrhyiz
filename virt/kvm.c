#include "capmix/virt.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/arch.h"
#include "capmix/obs.h"

#include <string.h>

static cm_vm_t g_vm[CM_MAX_VMS];
static cm_id_t g_vid = 1;
static uint64_t g_sleep_bias;

cm_error_t cm_virt_init(void)
{
    extern void cm_job_init(void);
    cm_job_init();
    memset(g_vm, 0, sizeof(g_vm));
    g_vid = 1;
    g_sleep_bias = 0;
    kinfo("virt: Type-1 KVM + VFIO/SR-IOV stubs, job/cgroup v2 ready");
    return CM_OK;
}

cm_vm_t *cm_vm_create(const char *name, cm_job_t *job, uint64_t mem_pages, int nvcpu)
{
    if (mem_pages == 0 || nvcpu <= 0 || nvcpu > CM_MAX_VCPU) return NULL;
    if (job && cm_cg_charge(job, mem_pages) != CM_OK) return NULL;
    if (cm_commit_charge(mem_pages) != CM_OK) {
        if (job) cm_cg_uncharge(job, mem_pages);
        return NULL;
    }
    for (int i = 0; i < CM_MAX_VMS; i++) {
        if (g_vm[i].used) continue;
        memset(&g_vm[i], 0, sizeof(g_vm[i]));
        g_vm[i].used = 1;
        g_vm[i].id = g_vid++;
        g_vm[i].name = name ? name : "vm";
        g_vm[i].job = job;
        g_vm[i].mem_pages = mem_pages;
        g_vm[i].guest_mem = cm_page_alloc(0);
        g_vm[i].nvcpu = nvcpu;
        for (int v = 0; v < nvcpu; v++) {
            g_vm[i].vcpu[v].used = 1;
            g_vm[i].vcpu[v].rip = 0xfff0; /* reset vector stand-in */
        }
        kinfo("vm: '%s' id=%llu mem=%llu pages vcpu=%d job=%s",
              g_vm[i].name, (unsigned long)g_vm[i].id,
              (unsigned long)mem_pages, nvcpu,
              job ? job->name : "-");
        return &g_vm[i];
    }
    cm_commit_uncharge(mem_pages);
    if (job) cm_cg_uncharge(job, mem_pages);
    return NULL;
}

cm_error_t cm_vcpu_run(cm_vm_t *vm, int vcpu)
{
    if (!vm || vcpu < 0 || vcpu >= vm->nvcpu || !vm->vcpu[vcpu].used)
        return CM_ERR_INVAL;
    /* Both builds: synthetic accounting demo, NOT a hardware guest entry. */
    vm->vcpu[vcpu].running = 1;
    vm->vcpu[vcpu].rip += 2;
    vm->vcpu[vcpu].exits++;
    vm->vmexits++;
    vm->vcpu[vcpu].running = 0;
    cm_obs_emit(cm_trace_new(), CM_EV_SCHED_SWITCH, vm->id, (uint64_t)vcpu, vm->vmexits);
    return CM_OK;
}

cm_error_t cm_vfio_attach(cm_vm_t *vm, int pci_bdf)
{
    if (!vm || vm->nvfio >= CM_MAX_VFIO) return CM_ERR_NOSPC;
    vm->vfio[vm->nvfio++] = pci_bdf;
    kinfo("vfio: attach bdf=%x → vm '%s' (accounting stub; no passthrough)", pci_bdf, vm->name);
    return CM_OK;
}

uint64_t cm_continuous_time(void)
{
    /* Sleep would add to g_sleep_bias instead of jumping the clock. */
    return cm_ticks_now() + g_sleep_bias;
}

extern void cm_job_dump(void);

void cm_virt_dump(void)
{
    cm_job_dump();
    for (int i = 0; i < CM_MAX_VMS; i++) {
        if (!g_vm[i].used) continue;
        kinfo("  vm %llu '%s' mem=%llu vcpu=%d exits=%llu vfio=%d",
              (unsigned long)g_vm[i].id, g_vm[i].name,
              (unsigned long)g_vm[i].mem_pages, g_vm[i].nvcpu,
              (unsigned long)g_vm[i].vmexits, g_vm[i].nvfio);
    }
}
