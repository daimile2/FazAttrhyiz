#include "capmix/kernel.h"
#include "capmix/vm.h"
#include <string.h>

#define VERIFY(expr) do { if (!(expr)) { \
    kerr("phase1: %s:%d: %s", __func__, __LINE__, #expr); failed = 1; goto cleanup; \
} } while (0)

static void close_handles(cm_task_t *task)
{
    if (task) for (int i = 1; i < CM_HANDLES_PER_TASK; i++)
        if (cm_handle_lookup(task, i)) cm_handle_drop(task, i);
}
static int connect(cm_task_t *sender, cm_task_t *receiver, int *a, int *b)
{
    int peer;
    if (cm_channel_create(sender, a, &peer)) return -1;
    cm_error_t err = cm_handle_duplicate(sender, peer, receiver, b, CM_RIGHT_ALL);
    cm_handle_drop(sender, peer);
    return err;
}
static uint64_t current_cr3(void)
{
#ifndef CAPMIX_HOSTED
    uint64_t value;
    __asm__ volatile("movq %%cr3, %0" : "=r"(value));
    return value;
#else
    return cm_paging_cr3();
#endif
}
static uint8_t *mapped_pointer(cm_vm_space_t *space, uint64_t virt)
{
#ifdef CAPMIX_HOSTED
    return cm_vm_space_pointer(space, virt);
#else
    CM_UNUSED(space);
    return (uint8_t *)(uintptr_t)virt;
#endif
}
/* Freestanding really dereferences each task's virtual mapping under its
 * own CR3. Hosted dereferences the corresponding backing frames explicitly. */
static int pattern(cm_vm_space_t *space, uint64_t base, uint64_t bytes, int write,
                    uint64_t *checksum)
{
    uint64_t flags = cm_irq_save(), home = current_cr3(), sum = 0;
    int bad = 0;
    cm_space_load(space->cr3);
    for (uint64_t off = 0; off < bytes; off += CM_PAGE_SIZE) {
        volatile uint8_t *p = mapped_pointer(space, base + off);
        if (!p) { bad = 1; break; }
        for (uint64_t j = 0; j < CM_PAGE_SIZE; j++) {
            uint8_t expected = (uint8_t)(((off + j) * 37 + ((off + j) >> 8)) ^ 0xa5);
            if (write) p[j] = expected;
            if (p[j] != expected) bad = 1;
            sum += p[j];
        }
    }
    cm_space_load(home);
    cm_irq_restore(flags);
    if (checksum) *checksum = sum;
    return bad;
}
static int change_and_read(cm_vm_space_t *sender, cm_vm_space_t *receiver,
                            uint64_t sender_va, uint64_t receiver_va)
{
    uint64_t flags = cm_irq_save(), home = current_cr3();
    cm_space_load(receiver->cr3);
    volatile uint8_t *r = mapped_pointer(receiver, receiver_va);
    r[0] = 0x6d;
    cm_space_load(sender->cr3);
    volatile uint8_t *s = mapped_pointer(sender, sender_va);
    int same = s[0] == 0x6d;
    cm_space_load(home);
    cm_irq_restore(flags);
    return same;
}
static int vmo_transfer_test(void)
{
    int failed = 0;
    cm_task_t *s = cm_task_create("vmo-sender", CM_QOS_DEFAULT);
    cm_task_t *r = cm_task_create("vmo-receiver", CM_QOS_DEFAULT);
    cm_vm_space_t sv = {0}, rv = {0};
    uint64_t free_before = cm_mm_free_pages(), committed_before, limit;
    extern void cm_commit_stats(uint64_t *, uint64_t *);
    cm_commit_stats(&committed_before, &limit);
    const uint64_t sva = 0x1000000, rva = 0x2000000, bytes = 16 * CM_PAGE_SIZE;
    int a, b, vmo, smap, rmap, occupied;
    cm_ipc_msg_t msg = {0}, received = {0};
    VERIFY(s && r);
    VERIFY(connect(s, r, &a, &b) == 0);
    VERIFY(cm_vmar_create(s, sva, 0x200000, &smap) == CM_OK);
    VERIFY(cm_vmar_create(r, rva, 0x200000, &rmap) == CM_OK);
    VERIFY(cm_vmo_create(s, bytes, &vmo) == CM_OK);
    cm_object_t *identity = cm_handle_lookup(s, vmo)->obj;
    cm_rights_t original_rights = cm_handle_lookup(s, vmo)->rights;
    VERIFY(cm_vmar_map(s, smap, vmo, sva, 0, bytes) == CM_OK);
    VERIFY(cm_vmar_acquire_space(s, smap, &sv) == CM_OK);
    uint64_t sent_sum, got_sum;
    VERIFY(pattern(&sv, sva, bytes, 1, &sent_sum) == 0);
    /* Occupy the source handle's numeric slot in receiver to prove rebinding. */
    while (!cm_handle_lookup(r, vmo))
        VERIFY(cm_handle_duplicate(s, vmo, r, &occupied, CM_RIGHT_INSPECT) == CM_OK);
    msg.opcode = 0x564d4f; msg.nhandles = 1; msg.handles[0] = vmo;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_OK);
    VERIFY(cm_handle_lookup(s, vmo) == NULL);
    VERIFY(cm_ipc_recv(r, b, &received) == CM_OK);
    VERIFY(received.nhandles == 1 && received.handles[0] != vmo);
    int rh = received.handles[0];
    VERIFY(cm_handle_lookup(r, rh)->obj == identity);
    VERIFY(cm_handle_lookup(r, rh)->rights == original_rights);
    uint64_t size;
    VERIFY(cm_vmo_size(r, rh, &size) == CM_OK && size == bytes);
    VERIFY(cm_vmar_map(r, rmap, rh, rva, 0, bytes) == CM_OK);
    VERIFY(cm_vmar_acquire_space(r, rmap, &rv) == CM_OK);
    for (uint64_t off = 0; off < bytes; off += CM_PAGE_SIZE) {
        uint64_t sp = cm_space_lookup(sv.root, sva + off);
        uint64_t rp = cm_space_lookup(rv.root, rva + off);
        VERIFY((sp & 0x000ffffffffff000ull) == (rp & 0x000ffffffffff000ull));
        VERIFY((sp & 7) == 7 && (rp & 7) == 7);
    }
    VERIFY(pattern(&rv, rva, bytes, 0, &got_sum) == 0 && sent_sum == got_sum);
    VERIFY(cm_handle_drop(r, rh) == CM_OK);
    VERIFY(change_and_read(&sv, &rv, sva + bytes - CM_PAGE_SIZE, rva + bytes - CM_PAGE_SIZE));
    VERIFY(cm_vmar_map(r, rmap, occupied, rva + bytes, 0, CM_PAGE_SIZE) == CM_ERR_DENIED);
    /* Close VMAR handles while leases still hold the mappings alive. */
    VERIFY(cm_handle_drop(s, smap) == CM_OK && cm_handle_drop(r, rmap) == CM_OK);
    VERIFY(change_and_read(&sv, &rv, sva, rva));
    kinfo("vmo transfer ok: bytes=%llu sender_va=%llx receiver_va=%llx sender_h=%d receiver_h=%d",
          (unsigned long)bytes, (unsigned long)sva, (unsigned long)rva, vmo, rh);
    kinfo("vmo zero-copy: shared backing=%u pages checksum=%llu shared-write ok",
          16u, (unsigned long)got_sum);
cleanup:
    cm_vmar_release_space(&sv); cm_vmar_release_space(&rv);
    close_handles(s); close_handles(r);
    uint64_t committed_after;
    cm_commit_stats(&committed_after, &limit);
    if (free_before != cm_mm_free_pages() || committed_before != committed_after) {
        kerr("vmo lifecycle: page/commit leak before=%llu after=%llu commit=%llu/%llu",
             (unsigned long)free_before, (unsigned long)cm_mm_free_pages(),
             (unsigned long)committed_before, (unsigned long)committed_after);
        failed = 1;
    } else if (!failed) kinfo("vmo lifecycle ok: mapping pins, pages and commit reclaimed");
    return failed;
}
static int transfer_failure_test(void)
{
    int failed = 0;
    cm_task_t *s = cm_task_create("vmo-errors-s", CM_QOS_DEFAULT);
    cm_task_t *r = cm_task_create("vmo-errors-r", CM_QOS_DEFAULT);
    cm_vm_space_t view = {0};
    uint64_t free_before = cm_mm_free_pages();
    int a, b, vmo, region, restricted;
    cm_ipc_msg_t msg = {0}, out = {0};
    VERIFY(s && r && connect(s, r, &a, &b) == 0);
    VERIFY(cm_vmo_create(s, 0, &vmo) == CM_ERR_INVAL);
    VERIFY(cm_vmo_create(s, UINT64_MAX, &vmo) == CM_ERR_INVAL);
    VERIFY(cm_vmo_create(s, 2 * CM_PAGE_SIZE, &vmo) == CM_OK);
    VERIFY(cm_vmar_create(s, 0x3000000, 0x200000, &region) == CM_OK);
    VERIFY(cm_handle_duplicate(s, vmo, s, &restricted, CM_RIGHT_READ | CM_RIGHT_MAP) == CM_OK);
    VERIFY(cm_vmar_map(s, region, restricted, 0x3000000, 0, CM_PAGE_SIZE) == CM_ERR_DENIED);
    VERIFY(cm_vmar_map(s, region, vmo, 0x3000001, 0, CM_PAGE_SIZE) == CM_ERR_INVAL);
    VERIFY(cm_vmar_map(s, region, vmo, 0x3000000, 2 * CM_PAGE_SIZE, CM_PAGE_SIZE) == CM_ERR_INVAL);
    VERIFY(cm_vmar_map(s, region, vmo, UINT64_MAX - 4095, 0, CM_PAGE_SIZE) == CM_ERR_INVAL);
    VERIFY(cm_vmar_map(s, region, vmo, 0x3000000, 0, CM_PAGE_SIZE) == CM_OK);
    VERIFY(cm_vmar_map(s, region, vmo, 0x3000000, 0, CM_PAGE_SIZE) == CM_ERR_EXIST);
    VERIFY(cm_vmar_acquire_space(s, region, &view) == CM_OK);
    VERIFY(cm_vmar_map(s, region, vmo, 0x3001000, 0, CM_PAGE_SIZE) == CM_ERR_BUSY);
    cm_vmar_release_space(&view);
    msg.nhandles = CM_IPC_MAX_HANDLES + 1;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_ERR_INVAL);
    msg.nhandles = 1; msg.handles[0] = restricted;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_ERR_DENIED && cm_handle_lookup(s, restricted));
    msg.handles[0] = CM_HANDLES_PER_TASK;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_ERR_INVAL);
    msg.nhandles = 2; msg.handles[0] = msg.handles[1] = vmo;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_ERR_INVAL && cm_handle_lookup(s, vmo));
    int readonly_endpoint, writeonly_endpoint;
    VERIFY(cm_handle_duplicate(s, a, s, &readonly_endpoint, CM_RIGHT_READ) == CM_OK);
    VERIFY(cm_handle_duplicate(r, b, r, &writeonly_endpoint, CM_RIGHT_WRITE) == CM_OK);
    msg.nhandles = 0;
    VERIFY(cm_ipc_send(s, readonly_endpoint, &msg) == CM_ERR_DENIED);
    VERIFY(cm_ipc_recv(r, writeonly_endpoint, &out) == CM_ERR_DENIED);
    for (unsigned i = 0; i < 8; i++) VERIFY(cm_ipc_send(s, a, &msg) == CM_OK);
    msg.nhandles = 1; msg.handles[0] = vmo;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_ERR_AGAIN && cm_handle_lookup(s, vmo));
    for (unsigned i = 0; i < 8; i++) VERIFY(cm_ipc_recv(r, b, &out) == CM_OK);
    /* Two handles must install together; one free slot is insufficient. */
    int copy;
    VERIFY(cm_handle_duplicate(s, vmo, s, &copy, CM_RIGHT_READ | CM_RIGHT_MAP | CM_RIGHT_TRANSFER) == CM_OK);
    msg.nhandles = 2; msg.handles[0] = vmo; msg.handles[1] = copy;
    cm_object_t *identity = cm_handle_lookup(s, vmo)->obj;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_OK);
    VERIFY(!cm_handle_lookup(s, vmo) && !cm_handle_lookup(s, copy));
    int filler;
    while ((filler = cm_handle_install(r, identity, CM_RIGHT_INSPECT)) > 0) {}
    VERIFY(filler == CM_ERR_NOSPC);
    int first_free = 0, second_free = 0;
    for (int i = 1; i < CM_HANDLES_PER_TASK; i++) {
        if (i == b || i == writeonly_endpoint) continue;
        if (!first_free) first_free = i;
        else { second_free = i; break; }
    }
    VERIFY(first_free && second_free);
    VERIFY(cm_handle_drop(r, first_free) == CM_OK);
    memset(&out, 0xa5, sizeof(out));
    VERIFY(cm_ipc_recv(r, b, &out) == CM_ERR_NOSPC && out.nhandles == 0xa5a5);
    VERIFY(!cm_handle_lookup(r, first_free));
    VERIFY(cm_handle_drop(r, second_free) == CM_OK);
    VERIFY(cm_ipc_recv(r, b, &out) == CM_OK && out.nhandles == 2);
    VERIFY(cm_handle_lookup(r, out.handles[0])->obj == identity);
    VERIFY(cm_handle_lookup(r, out.handles[1])->rights ==
           (CM_RIGHT_READ | CM_RIGHT_MAP | CM_RIGHT_TRANSFER));
    close_handles(r); /* receiver close, sender observes peer death */
    msg.nhandles = 0;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_ERR_DEAD);
    kinfo("vmo transfer rollback ok: invalid/rights/queue-full/handle-table-full/retry/peer-close");
cleanup:
    cm_vmar_release_space(&view);
    close_handles(s); close_handles(r);
    if (free_before != cm_mm_free_pages()) { kerr("vmo rollback: page leak"); failed = 1; }
    return failed;
}
static int queued_close_test(void)
{
    int failed = 0;
    cm_task_t *s = cm_task_create("vmo-close-s", CM_QOS_DEFAULT);
    cm_task_t *r = cm_task_create("vmo-close-r", CM_QOS_DEFAULT);
    uint64_t free_before = cm_mm_free_pages();
    int a, b, vmo;
    cm_ipc_msg_t msg = {0};
    VERIFY(s && r && connect(s, r, &a, &b) == 0);
    VERIFY(cm_vmo_create(s, 3 * CM_PAGE_SIZE, &vmo) == CM_OK);
    msg.nhandles = 1; msg.handles[0] = vmo;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_OK);
    VERIFY(cm_handle_drop(s, a) == CM_OK); /* queued data survives sender close */
    cm_ipc_msg_t out;
    VERIFY(cm_ipc_recv(r, b, &out) == CM_OK);
    VERIFY(cm_handle_drop(r, out.handles[0]) == CM_OK);
    VERIFY(cm_ipc_recv(r, b, &out) == CM_ERR_DEAD);
    close_handles(r);
    VERIFY(connect(s, r, &a, &b) == 0);
    VERIFY(cm_vmo_create(s, 3 * CM_PAGE_SIZE, &vmo) == CM_OK);
    msg.handles[0] = vmo;
    VERIFY(cm_ipc_send(s, a, &msg) == CM_OK);
    VERIFY(cm_handle_drop(r, b) == CM_OK); /* discard undelivered caps */
    kinfo("vmo queued lifetime ok: sender-close delivery and receiver-close reclaim");
cleanup:
    close_handles(s); close_handles(r);
    if (free_before != cm_mm_free_pages()) { kerr("vmo queued close: page leak"); failed = 1; }
    return failed;
}
static int allocation_rollback_test(void)
{
    int failed = 0;
    cm_task_t *task = cm_task_create("vmo-oom", CM_QOS_DEFAULT);
    cm_vm_space_t space = {0};
    uint64_t free_before = cm_mm_free_pages();
    extern void cm_commit_stats(uint64_t *, uint64_t *);
    uint64_t committed_before, limit, reserved = 0;
    cm_commit_stats(&committed_before, &limit);
    /* Drain the arena in large blocks, with a bounded static bookkeeping list. */
    static struct { void *page; unsigned order; } held[256];
    unsigned count = 0;
    void *spare = NULL;
    int vmo, region, failed_hid = -1;
    VERIFY(task);
    reserved = limit - committed_before;
    VERIFY(cm_commit_charge(reserved) == CM_OK);
    VERIFY(cm_vmo_create(task, CM_PAGE_SIZE, &failed_hid) == CM_ERR_OVERCOMMIT);
    cm_commit_uncharge(reserved); reserved = 0;
    VERIFY(cm_vmo_create(task, 2 * CM_PAGE_SIZE, &vmo) == CM_OK);
    VERIFY(cm_vmar_create(task, 0x4000000, 0x200000, &region) == CM_OK);
    spare = cm_page_alloc(1);
    VERIFY(spare);
    for (int order = CM_BUDDY_MAX_ORDER; order >= 0; order--) {
        void *p;
        while (count < CM_ARRAY_LEN(held) && (p = cm_page_alloc((unsigned)order)))
            { held[count].page = p; held[count].order = (unsigned)order; count++; }
    }
    VERIFY(cm_mm_free_pages() == 0);
    VERIFY(cm_vmo_create(task, CM_PAGE_SIZE, &failed_hid) == CM_ERR_NOMEM);
    cm_page_free(spare, 1); spare = NULL;
    VERIFY(cm_mm_free_pages() == 2);
    /* Two free pages permit metadata+one backing page, then force rollback. */
    VERIFY(cm_vmo_create(task, 2 * CM_PAGE_SIZE, &failed_hid) == CM_ERR_NOMEM);
    VERIFY(cm_mm_free_pages() == 2 && failed_hid == -1);
    /* Mapping requires >=3 page-table pages: force a partial-table failure. */
    VERIFY(cm_vmar_map(task, region, vmo, 0x4000000, 0, 2 * CM_PAGE_SIZE) == CM_ERR_NOMEM);
    VERIFY(cm_mm_free_pages() == 2);
    while (count) { count--; cm_page_free(held[count].page, held[count].order); }
    VERIFY(cm_vmar_acquire_space(task, region, &space) == CM_OK);
    VERIFY(!cm_space_lookup(space.root, 0x4000000) && !cm_space_lookup(space.root, 0x4001000));
    cm_vmar_release_space(&space);
    VERIFY(cm_vmar_map(task, region, vmo, 0x4000000, 0, 2 * CM_PAGE_SIZE) == CM_OK);
    kinfo("vmo allocation rollback ok: commit-denied/backing-OOM/page-table-OOM/retry");
cleanup:
    if (reserved) cm_commit_uncharge(reserved);
    if (spare) cm_page_free(spare, 1);
    while (count) { count--; cm_page_free(held[count].page, held[count].order); }
    cm_vmar_release_space(&space);
    close_handles(task);
    uint64_t committed_after;
    cm_commit_stats(&committed_after, &limit);
    if (free_before != cm_mm_free_pages() || committed_before != committed_after) {
        kerr("vmo allocation rollback: page/commit leak"); failed = 1;
    }
    return failed;
}

int cm_phase1_selftest(void)
{
    kinfo("--- selftest: phase1 VMO/VMAR zero-copy IPC ---");
    int failed = vmo_transfer_test() + transfer_failure_test() + queued_close_test() + allocation_rollback_test();
    kinfo("phase1 selftest: %s (%d failures)", failed ? "FAIL" : "PASS", failed);
    return failed;
}
