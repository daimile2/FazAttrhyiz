#ifndef CAPMIX_VIRT_H
#define CAPMIX_VIRT_H

#include "types.h"
#include "sched.h"
#include "virt_hw.h"

/* Phase 7: Type-1 hypervisor (KVM route) + VFIO, and Fuchsia-style
 * job objects that swallow Linux's seven namespaces + cgroup v2.
 */

#define CM_MAX_JOBS     16
#define CM_JOB_MEMBERS  16
#define CM_MAX_VMS      4
#define CM_MAX_VCPU     4
#define CM_MAX_VFIO     8

#define CM_NS_PID  (1u << 0)
#define CM_NS_MNT  (1u << 1)
#define CM_NS_NET  (1u << 2)
#define CM_NS_UTS  (1u << 3)
#define CM_NS_IPC  (1u << 4)
#define CM_NS_USER (1u << 5)
#define CM_NS_CGROUP (1u << 6)
#define CM_NS_ALL  0x7Fu

typedef struct {
    uint64_t memory_max;     /* pages, 0 = unlimited */
    uint64_t memory_cur;
    uint32_t cpu_weight;     /* 1..10000, default 100 */
    uint64_t io_max_bps;     /* 0 = unlimited */
    uint64_t io_cur;
} cm_cg_t;

typedef struct cm_job {
    int          used;
    cm_id_t      id;
    const char  *name;
    uint32_t     ns;         /* one object, not seven namespaces */
    cm_cg_t      cg;
    cm_id_t      members[CM_JOB_MEMBERS];
    int          nmembers;
    int          killed;
    cm_object_t *self_obj;
} cm_job_t;

typedef struct {
    int      used;
    int      running;
    uint64_t exits;
    uint64_t rip;
} cm_vcpu_t;

typedef struct {
    int         used;
    cm_id_t     id;
    const char *name;
    cm_job_t   *job;
    uint64_t    mem_pages;
    void       *guest_mem;
    cm_vcpu_t   vcpu[CM_MAX_VCPU];
    int         nvcpu;
    int         vfio[CM_MAX_VFIO];
    int         nvfio;
    uint64_t    vmexits;
} cm_vm_t;

cm_error_t cm_virt_init(void);

cm_job_t  *cm_job_create(const char *name, uint32_t ns);
cm_error_t cm_job_insert(cm_job_t *j, cm_id_t pid);
cm_error_t cm_job_kill(cm_job_t *j);          /* coalition: others untouched */
cm_error_t cm_cg_charge(cm_job_t *j, uint64_t pages);
void       cm_cg_uncharge(cm_job_t *j, uint64_t pages);
cm_job_t  *cm_job_lookup(cm_id_t id);

cm_vm_t   *cm_vm_create(const char *name, cm_job_t *job, uint64_t mem_pages, int nvcpu);
cm_error_t cm_vcpu_run(cm_vm_t *vm, int vcpu);
cm_error_t cm_vfio_attach(cm_vm_t *vm, int pci_bdf);
void       cm_virt_dump(void);
uint64_t cm_virt_real_vmexits(void);
void cm_virt_record_real_exit(void);

uint64_t   cm_continuous_time(void);          /* does not jump across "sleep" */

#endif
