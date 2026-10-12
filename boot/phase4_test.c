#include "capmix/kernel.h"
#include "capmix/io.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/blockd_wait_elf.inc"

#define CHECK(expr) do { if (!(expr)) { \
    kerr("phase4: %s line %d: %s", __func__, __LINE__, #expr); failed++; goto cleanup; \
} } while (0)
_Static_assert(sizeof(cm_ipc_msg_t) == 96 && offsetof(cm_ipc_msg_t, data) == 16, "blockd message ABI");
_Static_assert(sizeof(cm_drv_irq_packet_t) == 24, "blockd IRQ packet ABI");
_Static_assert(CM_SYS_IPC_RECV == 7 && CM_SYS_WRITE == 2 && CM_SYS_EXIT == 1, "blockd syscall ABI");
extern void cm_blk_set_offline(int off);

static void close_handles(cm_task_t *t)
{
    if (t) for (int h = 1; h < CM_HANDLES_PER_TASK; h++)
        if (cm_handle_lookup(t, h)) cm_handle_drop(t, h);
}
static void unused_driver(void) { cm_syscall(CM_SYS_EXIT, 0, 0, 0, 0); }
static int write_fixture(void)
{
    cm_file_t *file;
    cm_error_t err = cm_vfs_open("/bin/blockd.elf", CM_O_WRONLY | CM_O_CREAT | CM_O_TRUNC, &file);
    if (err) return err;
    int64_t n = cm_vfs_write(file, cm_blockd_wait_elf, sizeof(cm_blockd_wait_elf));
    err = n == (int64_t)sizeof(cm_blockd_wait_elf) ? cm_vfs_fsync(file) : CM_ERR_IO;
    cm_vfs_close(file);
    return err;
}
static int user_receive_test(void)
{
    int failed = 0, did = -1;
    cm_id_t pid = 0;
    cm_proc_t *p = NULL;
    CHECK(write_fixture() == CM_OK);
    cm_spawn_attr_t attr = { .path = "/bin/blockd.elf", .qos = CM_QOS_USER_INTERACTIVE };
    CHECK(cm_posix_spawn(&attr, &pid) == CM_OK);
    p = cm_proc_lookup(pid);
    CHECK(p && p->elf);
    int send, recv;
    CHECK(cm_channel_create(p->task, &send, &recv) == CM_OK);
    did = cm_drv_bind_channel("com.capmix.blockd", 12, pid, send, recv);
    CHECK(did >= 0);
    CHECK(cm_handle_drop(p->task, send) == CM_OK);
    uint32_t *config = cm_elf_pointer(p->elf, 0x401000);
    CHECK(config);
    config[0] = (uint32_t)recv; config[1] = (uint32_t)did;
#ifdef CAPMIX_HOSTED
    cm_irq_inject(12);
    CHECK(cm_drv_get(did)->delivered == 0); /* top half did not IPC */
    CHECK(cm_irq_softirq_flush() == 1);
    CHECK(cm_drv_get(did)->delivered == 1 && cm_drv_get(did)->received == 0);
    cm_ipc_msg_t message;
    CHECK(cm_drv_receive(pid, recv, &message) == CM_OK);
    cm_drv_irq_packet_t packet;
    memcpy(&packet, message.data, sizeof(packet));
    CHECK(message.opcode == CM_DRV_IRQ_EVENT && packet.irq == 12 && packet.driver_id == (uint32_t)did);
    CHECK(packet.sequence == 1 && packet.count == 1);
    CHECK(cm_drv_receive(pid, recv, &message) == CM_ERR_AGAIN);
    (void)cm_sched_run_until_idle(64);
    CHECK(p->exited && p->exit_code == CM_ERR_NOTSUP);
    cm_drv_process_exit(pid); /* hosted ELF skip does not execute EXIT syscall */
    kinfo("driver irq hosted: channel receive verified; CPL=3 execution skipped");
#else
    for (unsigned step = 0; step < 64 && p->main->state != CM_TH_BLOCKED && !p->exited; step++)
        (void)cm_sched_run_until_idle(1);
    CHECK(p->main->state == CM_TH_BLOCKED && cm_drv_get(did)->wait_endpoint);
    CHECK(cm_drv_get(did)->waits == 1 && cm_drv_get(did)->received == 0);
    kinfo("driver wait verified: sleeping CPL=3 before IRQ");
    cm_irq_inject(12);
    CHECK(cm_drv_get(did)->delivered == 0 && p->main->state == CM_TH_BLOCKED);
    CHECK(cm_irq_softirq_flush() == 1);
    CHECK(p->main->state == CM_TH_RUNNABLE && cm_drv_get(did)->delivered == 1);
    kinfo("driver wait verified: IRQ -> wake RUNNABLE");
    for (unsigned step = 0; step < 64 && (!p->exited || (p->main && p->main->id)); step++)
        (void)cm_sched_run_until_idle(1);
    CHECK(p->exited && p->exit_code == 0 && p->elf_started);
    uint64_t *report = cm_elf_pointer(p->elf, 0x401010);
    CHECK(report && report[0] == 1 && report[1] == 3);
    CHECK(p->elf_write_calls == 1 && p->elf_written_bytes == 19);
    CHECK(cm_drv_get(did)->received == 1 && cm_drv_get(did)->wakes == 1);
    kinfo("driver irq user ok: com.capmix.blockd CPL=3 recv_count=1 exit=0");
#endif
    CHECK(cm_drv_get(did)->state == CM_DRV_OFFLINE && !cm_blk_online());
    CHECK(!cm_handle_lookup(p->task, recv));
    uint64_t delivered = cm_drv_get(did)->delivered;
    cm_irq_inject(12); (void)cm_irq_softirq_flush();
    CHECK(cm_drv_get(did)->delivered == delivered);
    CHECK(cm_proc_reap_elf(pid) == CM_OK);
    kinfo("driver exit isolation ok: device offline, IRQ delivery stopped, image reclaimed");
cleanup:
    if (p && !p->exited && did >= 0) cm_drv_crash(did);
    if (p && p->main && p->main->id && p->main->state == CM_TH_DEAD) cm_thread_reap(p->main);
    if (pid) (void)cm_proc_reap_elf(pid);
    return failed;
}
/* One live native test thread at a time; no helper races the serial ELF stack. */
static cm_id_t wait_pid;
static int wait_handle;
static cm_error_t wait_result;
static void native_wait_driver(void)
{
    cm_ipc_msg_t message;
    wait_result = cm_drv_receive_wait(wait_pid, wait_handle, &message);
    cm_syscall(CM_SYS_EXIT, 0, 0, 0, 0);
}
static int wait_lifecycle_test(unsigned mode)
{
    int failed = 0, did = -1, send, recv;
    cm_id_t pid = 0;
    cm_proc_t *p = NULL;
    cm_spawn_attr_t attr = { .path = "blockd-wait-lifecycle", .entry = native_wait_driver,
                             .qos = CM_QOS_USER_INTERACTIVE };
    CHECK(cm_posix_spawn(&attr, &pid) == CM_OK);
    p = cm_proc_lookup(pid);
    CHECK(p && cm_channel_create(p->task, &send, &recv) == CM_OK);
    did = cm_drv_bind_channel("blockd-wait-lifecycle", 12, pid, send, recv);
    CHECK(did >= 0);
    CHECK(cm_handle_drop(p->task, send) == CM_OK);
    wait_pid = pid; wait_handle = recv; wait_result = CM_ERR_BUSY;
    for (unsigned step = 0; step < 64 && p->main->state != CM_TH_BLOCKED && !p->exited; step++)
        (void)cm_sched_run_until_idle(1);
    cm_driver_t *d = cm_drv_get(did);
    CHECK(p->main->state == CM_TH_BLOCKED && d->wait_endpoint && d->waits == 1);
    CHECK(wait_result == CM_ERR_BUSY);
    if (mode == 0) {
        cm_irq_inject(12);
        CHECK(p->main->state == CM_TH_BLOCKED && !d->delivered);
        CHECK(cm_irq_softirq_flush() == 1 && p->main->state == CM_TH_RUNNABLE);
    } else if (mode == 1) {
        CHECK(cm_drv_wait_cancel(pid) == CM_OK && p->main->state == CM_TH_RUNNABLE);
    } else if (mode == 2) {
        CHECK(cm_handle_drop(p->task, recv) == CM_OK);
        (void)cm_irq_softirq_flush(); /* lifecycle retry detects receiver close and wakes */
        cm_ipc_msg_t message;
        CHECK(cm_drv_receive(pid, recv, &message) == CM_ERR_DEAD);
        CHECK(p->main->state == CM_TH_RUNNABLE && !d->wait_endpoint);
    } else {
        cm_irq_inject(12); /* stale pending top half must be discarded after crash */
        CHECK(cm_drv_crash(did) == CM_OK);
        CHECK(p->main->state == CM_TH_DEAD && !d->wait_endpoint);
        (void)cm_irq_softirq_flush();
        CHECK(!d->delivered && !d->pending && wait_result == CM_ERR_BUSY);
        static uint8_t buffer[CM_BLK_SECTOR];
        CHECK(cm_blk_rw(CM_OP_READ, CM_BLK_RAW_LBA, 1, buffer) == CM_ERR_DEAD);
        CHECK(!cm_blk_online() && cm_drv_kernel_alive());
        kinfo("driver blocked crash isolation ok: pending discarded buddy usable kernel_alive=1");
    }
    if (mode < 3) {
        for (unsigned step = 0; step < 64 && (!p->exited || (p->main && p->main->id)); step++)
            (void)cm_sched_run_until_idle(1);
        CHECK(p->exited && !p->exit_code && !d->wait_endpoint);
        CHECK(wait_result == (mode == 0 ? CM_OK : mode == 1 ? CM_ERR_AGAIN : CM_ERR_DEAD));
        CHECK(d->wakes == 1 && d->received == (mode == 0 ? 1u : 0u));
        kinfo("driver wait lifecycle verified: %s result=%d recv_count=%llu",
              mode == 0 ? "sleep -> IRQ -> wake -> recv" : mode == 1 ? "cancel" : "peer-close",
              wait_result, (unsigned long)d->received);
    }
    CHECK(d->state == CM_DRV_OFFLINE && !cm_blk_online());
cleanup:
    if (p && !p->exited && did >= 0) cm_drv_crash(did);
    if (p && p->main && p->main->id && p->main->state == CM_TH_DEAD) cm_thread_reap(p->main);
    if (p) close_handles(p->task);
    return failed;
}
static void other_irq(int irq, void *arg) { CM_UNUSED(irq); (*(unsigned *)arg)++; }
static int backpressure_and_crash_test(void)
{
    int failed = 0, did = -1;
    cm_id_t pid = 0;
    cm_proc_t *p = NULL;
    cm_spawn_attr_t attr = { .path = "blockd-crash-test", .entry = unused_driver, .qos = CM_QOS_UTILITY };
    CHECK(cm_posix_spawn(&attr, &pid) == CM_OK);
    p = cm_proc_lookup(pid);
    CHECK(p && p->task);
    int send, recv, readonly, unrelated_a, unrelated_b;
    CHECK(cm_channel_create(p->task, &send, &recv) == CM_OK);
    CHECK(cm_channel_create(p->task, &unrelated_a, &unrelated_b) == CM_OK);
    CHECK(cm_handle_duplicate(p->task, send, p->task, &readonly, CM_RIGHT_READ | CM_RIGHT_DUPLICATE) == CM_OK);
    CHECK(cm_drv_bind_channel("bad-rights", 12, pid, readonly, recv) == CM_ERR_DENIED);
    CHECK(cm_drv_bind_channel("bad-pair", 12, pid, send, unrelated_b) == CM_ERR_INVAL);
    CHECK(cm_drv_bind_channel("bad-irq", -1, pid, send, recv) == CM_ERR_INVAL);
    unsigned other_hits = 0;
    CHECK(cm_irq_request(13, other_irq, &other_hits, "occupied") == CM_OK);
    CHECK(cm_drv_bind_channel("occupied", 13, pid, send, recv) == CM_ERR_BUSY);
    CHECK(cm_irq_release(13, other_irq, &other_hits) == CM_OK);
    did = cm_drv_bind_channel("blockd-crash-test", 12, pid, send, recv);
    CHECK(did >= 0 && cm_blk_online()); /* explicit registration reattaches device */
    CHECK(cm_drv_bind_channel("duplicate", 12, pid, send, recv) == CM_ERR_BUSY);
    cm_ipc_msg_t message;
    CHECK(cm_drv_receive(pid, unrelated_b, &message) == CM_ERR_DENIED);
    uint64_t overflow = cm_irq_overflow_count();
    for (unsigned i = 0; i < CM_SOFTIRQ_MAX + 3; i++) cm_irq_inject(12);
    CHECK(cm_irq_overflow_count() == overflow + 3);
    CHECK(cm_drv_get(did)->delivered == 0);
    CHECK(cm_irq_softirq_flush() == CM_SOFTIRQ_MAX);
    cm_driver_t *d = cm_drv_get(did);
    CHECK(d->delivered == 8 && d->pending == CM_SOFTIRQ_MAX - 8 && d->backpressure > 0);
    uint64_t events = 0;
    for (unsigned i = 0; i < 8; i++) {
        CHECK(cm_drv_receive(pid, recv, &message) == CM_OK);
        cm_drv_irq_packet_t packet;
        memcpy(&packet, message.data, sizeof(packet));
        CHECK(packet.sequence == events + 1 && packet.count == 1);
        events += packet.count;
    }
    CHECK(cm_irq_softirq_flush() == 0); /* retries even with no new top-half work */
    CHECK(d->pending == 0 && d->delivered == 9);
    CHECK(cm_drv_receive(pid, recv, &message) == CM_OK);
    cm_drv_irq_packet_t packet;
    memcpy(&packet, message.data, sizeof(packet));
    CHECK(packet.sequence == 9 && packet.count == CM_SOFTIRQ_MAX - 8);
    events += packet.count;
    CHECK(events == CM_SOFTIRQ_MAX);
    kinfo("driver irq backpressure ok: delivered=%llu events=%llu coalesced=%llu top_overflow=3",
          (unsigned long)d->delivered, (unsigned long)events, (unsigned long)packet.count);
    /* Pending top-half item must not invoke a new handler after unbind/rebind. */
    cm_irq_inject(12);
    CHECK(cm_handle_drop(p->task, recv) == CM_OK);
    int reused;
    CHECK(cm_handle_duplicate(p->task, unrelated_b, p->task, &reused, CM_RIGHT_READ) == CM_OK && reused == recv);
    CHECK(cm_drv_receive(pid, recv, &message) == CM_ERR_DEAD);
    CHECK(d->state == CM_DRV_OFFLINE); /* numeric reuse is not the registered capability */
    kinfo("driver irq peer-close ok: reused handle rejected, device offline");
    int crash_did = cm_drv_bind_channel("blockd-crash-rebind", 12, pid, unrelated_a, unrelated_b);
    CHECK(crash_did >= 0 && cm_blk_online());
    CHECK(cm_drv_receive(pid, unrelated_b, &message) == CM_ERR_AGAIN);
    CHECK(cm_drv_crash(crash_did) == CM_OK);
    CHECK(p->exited && p->exit_code == 11 && p->main->state == CM_TH_DEAD);
    CHECK(cm_drv_get(crash_did)->state == CM_DRV_OFFLINE && cm_drv_get(crash_did)->crashes == 1 && !cm_blk_online());
    CHECK(!cm_handle_lookup(p->task, recv));
    uint64_t stale = cm_irq_stale_count();
    CHECK(cm_irq_request(12, other_irq, &other_hits, "replacement") == CM_OK);
    CHECK(cm_irq_softirq_flush() == 1 && other_hits == 0 && cm_irq_stale_count() == stale + 1);
    CHECK(cm_irq_release(12, other_irq, &other_hits) == CM_OK);
    cm_irq_inject(12); (void)cm_irq_softirq_flush();
    CHECK(d->delivered == 9);
    static uint8_t buffer[CM_BLK_SECTOR];
    CHECK(cm_blk_rw(CM_OP_READ, CM_BLK_RAW_LBA, 1, buffer) == CM_ERR_DEAD);
    cm_sqe_t sq = { .opcode = CM_OP_READ, .addr = (uint64_t)(uintptr_t)buffer,
                    .len = sizeof(buffer), .off = 0, .user_data = 0x44 };
    cm_cqe_t cq;
    CHECK(cm_uring_submit(&sq) == CM_OK && cm_uring_complete(&cq) == CM_OK && cq.res == CM_ERR_DEAD);
    void *page = cm_page_alloc(0);
    CHECK(page);
    cm_page_free(page, 0);
    CHECK(cm_drv_kernel_alive());
    kinfo("driver crash isolation ok: task dead device offline I/O=DEAD buddy usable kernel_alive=1");
cleanup:
    if (p && !p->exited && did >= 0) cm_drv_crash(did);
    if (p && p->main && p->main->id && p->main->state == CM_TH_DEAD) cm_thread_reap(p->main);
    if (p) close_handles(p->task);
    return failed;
}
int cm_phase4_selftest(void)
{
    kinfo("--- selftest: phase4 IRQ channel -> user driver ---");
    int failed = user_receive_test();
    for (unsigned mode = 0; mode < 4; mode++) failed += wait_lifecycle_test(mode);
    failed += backpressure_and_crash_test();
    kinfo("phase4 selftest: %s (%d failures)", failed ? "FAIL" : "PASS", failed);
    return failed;
}
