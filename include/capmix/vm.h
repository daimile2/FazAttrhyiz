#ifndef CAPMIX_VM_H
#define CAPMIX_VM_H
#include "cap.h"

#define CM_VMO_MAX_PAGES 128u
#define CM_VMAR_MAX_MAPS 32u

/* Stage 1: eager resident VMO pages and task-owned, inactive VMAR building.
 * Only shared RW mappings are supported; rights must include READ|WRITE|MAP.
 * Existing spawn placeholders are not backed VMOs/VMARs and are rejected. */
cm_error_t cm_vmo_create(cm_task_t *task, uint64_t bytes, int *out_hid);
cm_error_t cm_vmo_size(cm_task_t *task, int hid, uint64_t *out_bytes);
cm_error_t cm_vmar_create(cm_task_t *task, uint64_t base, uint64_t bytes, int *out_hid);
cm_error_t cm_vmar_map(cm_task_t *task, int vmar_hid, int vmo_hid,
                      uint64_t virt, uint64_t offset, uint64_t bytes);
/* A space lease pins the VMAR and its mappings until all CPUs have left it.
 * Caller restores its former CR3 before releasing the lease. No user-mode
 * scheduler/KPTI entry integration is implied by this kernel-facing API. */
typedef struct {
    cm_object_t *object;
    uint64_t *root;
    uint64_t cr3;
} cm_vm_space_t;
cm_error_t cm_vmar_acquire_space(cm_task_t *task, int hid, cm_vm_space_t *out);
void cm_vmar_release_space(cm_vm_space_t *space);
/* Hosted dereference adapter: return the same backing page, never a copy.
 * The caller must hold a space lease; contiguous access stops at page end. */
void *cm_vm_space_pointer(const cm_vm_space_t *space, uint64_t virt);
#endif
