#include "capmix/vm.h"
#include "capmix/mm.h"
#include "capmix/numa.h"
#include "capmix/sched.h"
#include "capmix/arch.h"
#include <string.h>

#define PT_ADDR 0x000ffffffffff000ull
#define PT_PRESENT 1ull
#define PT_HUGE (1ull << 7)

typedef struct {
    uint64_t bytes, npages;
    void *pages[CM_VMO_MAX_PAGES];
} vmo_t;
typedef struct {
    cm_object_t *vmo;
    uint64_t virt, offset, bytes;
} mapping_t;
typedef struct {
    cm_task_t *owner;
    uint64_t *root;
    uint64_t cr3, base, bytes;
    unsigned count, leases;
    mapping_t maps[CM_VMAR_MAX_MAPS];
} vmar_t;
_Static_assert(sizeof(vmo_t) <= CM_PAGE_SIZE, "VMO metadata page");
_Static_assert(sizeof(vmar_t) <= CM_PAGE_SIZE, "VMAR metadata page");
static volatile unsigned g_vm_lock;
static uint64_t vm_lock(void)
{
    uint64_t flags = cm_irq_save();
    while (__sync_lock_test_and_set(&g_vm_lock, 1u))
        __asm__ volatile("pause" ::: "memory");
    return flags;
}
static void vm_unlock(uint64_t flags)
{
    __sync_lock_release(&g_vm_lock);
    cm_irq_restore(flags);
}
static uint64_t physical(void *p) { return (uint64_t)(uintptr_t)p - cm_hhdm(); }
static void *direct(uint64_t p) { return (void *)(uintptr_t)(p + cm_hhdm()); }

static void vmo_destroy(void *payload)
{
    vmo_t *v = payload;
    for (uint64_t i = 0; i < v->npages; i++) cm_page_free(v->pages[i], 0);
    cm_commit_uncharge(v->npages);
    cm_page_free(v, 0);
}
/* Private teardown of these standalone roots, never the shared kernel half.
 * No leaf frame is freed here: each backing frame belongs to its VMO. */
static void free_tables(uint64_t *table, unsigned level)
{
    if (level > 1) {
        for (unsigned i = 0; i < 512; i++)
            if ((table[i] & PT_PRESENT) && !(table[i] & PT_HUGE))
                free_tables(direct(table[i] & PT_ADDR), level - 1);
    }
    cm_page_free(table, 0);
}
static void vmar_destroy(void *payload)
{
    vmar_t *v = payload;
    for (unsigned i = 0; i < 256; i++)
        if (v->root[i] & PT_PRESENT)
            free_tables(direct(v->root[i] & PT_ADDR), cm_paging_levels() - 1);
    cm_page_free(v->root, 0);
    for (unsigned i = 0; i < v->count; i++) cm_obj_release(v->maps[i].vmo);
    cm_page_free(v, 0);
}
static cm_error_t acquire(cm_task_t *t, int hid, cm_obj_type_t type,
                           cm_rights_t rights, void (*destroy)(void *), cm_object_t **out)
{
    cm_error_t err = cm_handle_acquire(t, hid, type, rights, out);
    if (err) return err;
    if ((*out)->destroy != destroy) {
        cm_obj_release(*out);
        return CM_ERR_INVAL;
    }
    return CM_OK;
}
cm_error_t cm_vmo_create(cm_task_t *task, uint64_t bytes, int *out_hid)
{
    if (!task || !out_hid || !bytes || bytes > CM_VMO_MAX_PAGES * CM_PAGE_SIZE)
        return CM_ERR_INVAL;
    uint64_t pages = (bytes + CM_PAGE_SIZE - 1) / CM_PAGE_SIZE;
    uint64_t flags = vm_lock();
    cm_error_t err = cm_commit_charge(pages);
    if (err) { vm_unlock(flags); return err; }
    vmo_t *v = cm_page_alloc_node(task->memory_node, 0);
    if (!v) { cm_commit_uncharge(pages); vm_unlock(flags); return CM_ERR_NOMEM; }
    memset(v, 0, CM_PAGE_SIZE);
    v->bytes = pages * CM_PAGE_SIZE;
    for (; v->npages < pages; v->npages++) {
        v->pages[v->npages] = cm_page_alloc_node(task->memory_node, 0);
        if (!v->pages[v->npages]) {
            cm_commit_uncharge(pages - v->npages);
            vmo_destroy(v);
            vm_unlock(flags);
            return CM_ERR_NOMEM;
        }
        memset(v->pages[v->npages], 0, CM_PAGE_SIZE);
    }
    cm_rights_t rights = CM_RIGHT_READ | CM_RIGHT_WRITE | CM_RIGHT_MAP |
                        CM_RIGHT_TRANSFER | CM_RIGHT_DUPLICATE | CM_RIGHT_INSPECT;
    cm_object_t *obj = cm_obj_mint_owned(CM_OBJ_VMO, v, rights, vmo_destroy);
    if (!obj) { vmo_destroy(v); vm_unlock(flags); return CM_ERR_NOSPC; }
    int hid = cm_handle_install(task, obj, rights);
    cm_obj_release(obj);
    vm_unlock(flags);
    if (hid < 0) return hid;
    *out_hid = hid;
    return CM_OK;
}
cm_error_t cm_vmo_size(cm_task_t *task, int hid, uint64_t *out_bytes)
{
    if (!out_bytes) return CM_ERR_INVAL;
    cm_object_t *obj;
    cm_error_t err = acquire(task, hid, CM_OBJ_VMO, CM_RIGHT_INSPECT, vmo_destroy, &obj);
    if (err) return err;
    *out_bytes = ((vmo_t *)obj->payload)->bytes;
    cm_obj_release(obj);
    return CM_OK;
}
cm_error_t cm_vmar_create(cm_task_t *task, uint64_t base, uint64_t bytes, int *out_hid)
{
    if (!task || !out_hid || base < 0x10000 || !bytes ||
        ((base | bytes) & CM_PAGE_MASK) || base >= (1ull << 47) || bytes > (1ull << 47) - base)
        return CM_ERR_INVAL;
    vmar_t *v = cm_page_alloc_node(task->memory_node, 0);
    if (!v) return CM_ERR_NOMEM;
    memset(v, 0, CM_PAGE_SIZE);
    uint64_t flags = vm_lock();
    v->root = cm_space_create(&v->cr3);
    if (!v->root) { vm_unlock(flags); cm_page_free(v, 0); return CM_ERR_NOMEM; }
    v->owner = task; v->base = base; v->bytes = bytes;
    cm_rights_t rights = CM_RIGHT_MAP | CM_RIGHT_INSPECT;
    cm_object_t *obj = cm_obj_mint_owned(CM_OBJ_VMAR, v, rights, vmar_destroy);
    if (!obj) { vmar_destroy(v); vm_unlock(flags); return CM_ERR_NOSPC; }
    int hid = cm_handle_install(task, obj, rights);
    cm_obj_release(obj);
    vm_unlock(flags);
    if (hid < 0) return hid;
    *out_hid = hid;
    return CM_OK;
}
/* Rollback a newly-created leaf in an inactive private VMAR. This is the
 * only PTE edit outside the existing mapper; it never edits KPTI roots.
 * Reclaim empty tables so failed multi-page maps do not leak allocations. */
static int remove_leaf(uint64_t *table, unsigned level, uint64_t virt)
{
    unsigned index = (unsigned)((virt >> (12 + 9 * (level - 1))) & 511);
    if (level == 1) table[index] = 0;
    else if ((table[index] & PT_PRESENT) && !(table[index] & PT_HUGE)) {
        uint64_t *child = direct(table[index] & PT_ADDR);
        if (remove_leaf(child, level - 1, virt)) {
            table[index] = 0;
            cm_page_free(child, 0);
        }
    }
    for (unsigned i = 0; i < 512; i++) if (table[i] & PT_PRESENT) return 0;
    return 1;
}
cm_error_t cm_vmar_map(cm_task_t *task, int vmar_hid, int vmo_hid,
                      uint64_t virt, uint64_t offset, uint64_t bytes)
{
    if (!bytes || ((virt | offset | bytes) & CM_PAGE_MASK)) return CM_ERR_INVAL;
    cm_object_t *region, *memory;
    cm_error_t err = acquire(task, vmar_hid, CM_OBJ_VMAR, CM_RIGHT_MAP, vmar_destroy, &region);
    if (err) return err;
    err = acquire(task, vmo_hid, CM_OBJ_VMO,
                  CM_RIGHT_MAP | CM_RIGHT_READ | CM_RIGHT_WRITE, vmo_destroy, &memory);
    if (err) { cm_obj_release(region); return err; }
    vmar_t *v = region->payload;
    vmo_t *m = memory->payload;
    uint64_t flags = vm_lock();
    if (v->owner != task) err = CM_ERR_DENIED;
    else if (v->leases) err = CM_ERR_BUSY;
    else if (virt < v->base || virt - v->base >= v->bytes ||
             bytes > v->bytes - (virt - v->base) || offset >= m->bytes || bytes > m->bytes - offset)
        err = CM_ERR_INVAL;
    else if (v->count == CM_VMAR_MAX_MAPS) err = CM_ERR_NOSPC;
    if (!err) {
        for (uint64_t off = 0; off < bytes; off += CM_PAGE_SIZE)
            if (cm_space_lookup(v->root, virt + off)) { err = CM_ERR_EXIST; break; }
    }
    if (!err) {
        uint64_t off;
        for (off = 0; off < bytes; off += CM_PAGE_SIZE)
            if (cm_space_map(v->root, virt + off, physical(m->pages[(offset + off) / CM_PAGE_SIZE]), 1)) {
                err = CM_ERR_NOMEM; break;
            }
        if (err) {
            /* Include the failing address: mapper may have allocated a prefix. */
            for (uint64_t rollback = 0; rollback <= off; rollback += CM_PAGE_SIZE)
                remove_leaf(v->root, cm_paging_levels(), virt + rollback);
        } else {
            v->maps[v->count++] = (mapping_t){ memory, virt, offset, bytes };
            memory = NULL; /* acquired VMO ref now belongs to this mapping */
        }
    }
    vm_unlock(flags);
    cm_obj_release(memory);
    cm_obj_release(region);
    return err;
}
cm_error_t cm_vmar_acquire_space(cm_task_t *task, int hid, cm_vm_space_t *out)
{
    if (!out) return CM_ERR_INVAL;
    cm_object_t *obj;
    cm_error_t err = acquire(task, hid, CM_OBJ_VMAR, CM_RIGHT_INSPECT, vmar_destroy, &obj);
    if (err) return err;
    vmar_t *v = obj->payload;
    uint64_t flags = vm_lock();
    if (v->owner != task) err = CM_ERR_DENIED;
    else { v->leases++; *out = (cm_vm_space_t){obj, v->root, v->cr3}; }
    vm_unlock(flags);
    if (err) cm_obj_release(obj);
    return err;
}
void cm_vmar_release_space(cm_vm_space_t *space)
{
    if (!space || !space->object) return;
    uint64_t flags = vm_lock();
    vmar_t *v = space->object->payload;
    v->leases--;
    vm_unlock(flags);
    cm_obj_release(space->object);
    memset(space, 0, sizeof(*space));
}
void *cm_vm_space_pointer(const cm_vm_space_t *space, uint64_t virt)
{
    if (!space || !space->object) return NULL;
    uint64_t pte = cm_space_lookup(space->root, virt);
    if ((pte & 7) != 7) return NULL;
    return (uint8_t *)direct(pte & PT_ADDR) + (virt & CM_PAGE_MASK);
}
