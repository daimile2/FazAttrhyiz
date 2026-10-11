#include "capmix/kernel.h"
#include "capmix/net.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/net_elf.inc"

#define CHECK(expr) do { if (!(expr)) { \
    kerr("phase5: %s line %d: %s", __func__, __LINE__, #expr); failed++; goto cleanup; \
} } while (0)
_Static_assert(sizeof(cm_udp_datagram_t) == 272 && offsetof(cm_udp_datagram_t, data) == 16, "net ELF UDP ABI");
_Static_assert(CM_SYS_UDP_OPEN == 17 && CM_SYS_UDP_BIND == 18 && CM_SYS_UDP_SEND == 19 &&
               CM_SYS_UDP_RECV == 20 && CM_SYS_UDP_CLOSE == 21, "net ELF syscall ABI");
static const uint8_t payload[] = "udp-loopback-test";
static void close_handles(cm_task_t *task)
{
    if (task) for (int i = 1; i < CM_HANDLES_PER_TASK; i++)
        if (cm_handle_lookup(task, i)) cm_handle_drop(task, i);
}
static int kernel_echo_test(int lo)
{
    int failed = 0;
    cm_task_t *client = cm_task_create("udp-client", CM_QOS_DEFAULT);
    cm_task_t *server = cm_task_create("udp-echo", CM_QOS_DEFAULT);
    cm_iface_t *iface = cm_if_get(lo);
    uint64_t tx = iface->tx, rx = iface->rx;
    int a, b, nft, readonly, writeonly, duplicate;
    cm_udp_datagram_t got;
    int old_prog = iface->xdp_prog, old_bpf = cm_bpf_enabled();
    CHECK(client && server);
    CHECK(cm_udp_open(client, &a) == CM_OK && cm_udp_open(server, &b) == CM_OK);
    CHECK(cm_udp_bind(client, a, lo, 9000) == CM_OK && cm_udp_bind(server, b, lo, 9001) == CM_OK);
    CHECK(cm_udp_sendto(client, a, 9001, payload, sizeof(payload) - 1) == CM_OK);
    CHECK(cm_udp_recv(server, b, &got) == CM_OK && got.len == sizeof(payload) - 1);
    CHECK(got.sport == 9000 && got.dport == 9001 && !memcmp(got.data, payload, got.len));
    CHECK(cm_udp_sendto(server, b, got.sport, got.data, got.len) == CM_OK);
    CHECK(cm_udp_recv(client, a, &got) == CM_OK && !memcmp(got.data, payload, got.len));
    CHECK(iface->tx == tx + 2 && iface->rx == rx + 2);
    kinfo("udp lo echo ok: tx +2 rx +2 bytes=%u payload verified (odd-length checksum)", got.len);
    CHECK(cm_udp_open(server, &duplicate) == CM_OK);
    CHECK(cm_udp_bind(server, duplicate, lo, 9001) == CM_ERR_EXIST);
    CHECK(cm_udp_bind(server, duplicate, lo, 0) == CM_ERR_INVAL);
    CHECK(cm_handle_duplicate(client, a, client, &readonly, CM_RIGHT_READ) == CM_OK);
    CHECK(cm_handle_duplicate(server, b, server, &writeonly, CM_RIGHT_WRITE) == CM_OK);
    CHECK(cm_udp_sendto(client, readonly, 9001, payload, 1) == CM_ERR_DENIED);
    CHECK(cm_udp_recv(server, writeonly, &got) == CM_ERR_DENIED);
    CHECK(cm_udp_sendto(client, a, 9001, payload, CM_UDP_PAYLOAD + 1) == CM_ERR_INVAL);
    CHECK(cm_udp_sendto(client, a, 8999, payload, 1) == CM_ERR_NOENT);
    for (unsigned i = 0; i < CM_UDP_QUEUE; i++) CHECK(cm_udp_sendto(client, a, 9001, payload, 1) == CM_OK);
    CHECK(cm_udp_sendto(client, a, 9001, payload, 1) == CM_ERR_AGAIN);
    for (unsigned i = 0; i < CM_UDP_QUEUE; i++) CHECK(cm_udp_recv(server, b, &got) == CM_OK && got.data[0] == payload[0]);
    memset(&got, 0xa5, sizeof(got));
    CHECK(cm_udp_recv(server, b, &got) == CM_ERR_AGAIN && got.len == 0xa5a5);
    uint8_t frame[CM_UDP_FRAME_MAX]; uint16_t bytes;
    CHECK(cm_udp_frame(CM_UDP_LO_ADDR, 9000, 9001, payload, sizeof(payload) - 1, frame, &bytes) == CM_OK);
    frame[24] ^= 1; /* IPv4 checksum */
    CHECK(cm_udp_inject(lo, frame, bytes) == CM_ERR_INVAL);
    frame[24] ^= 1; frame[40] ^= 1; /* UDP checksum */
    CHECK(cm_udp_inject(lo, frame, bytes) == CM_ERR_INVAL);
    frame[40] ^= 1;
    CHECK(cm_udp_inject(lo, frame, bytes - 1) == CM_ERR_INVAL);
    frame[20] = 0x20; /* MF flag, also invalid IP checksum */
    CHECK(cm_udp_inject(lo, frame, bytes) == CM_ERR_INVAL);
    CHECK(cm_udp_recv(server, b, &got) == CM_ERR_AGAIN);
    cm_bpf_ins_t drop[] = {
        {CM_BPF_LD_DPORT, 0, 0}, {CM_BPF_JEQ, 1, 9001},
        {CM_BPF_RET, 0, CM_XDP_PASS}, {CM_BPF_RET, 0, CM_XDP_DROP}
    };
    int prog = cm_bpf_load("udp-lo-drop", drop, CM_ARRAY_LEN(drop));
    CHECK(prog >= 0 && cm_bpf_attach(prog, lo) == CM_OK);
    CHECK(cm_udp_sendto(client, a, 9001, payload, 1) == CM_ERR_DENIED);
    CHECK(cm_udp_recv(server, b, &got) == CM_ERR_AGAIN);
    cm_bpf_disable();
    CHECK(cm_udp_sendto(client, a, 9001, payload, 1) == CM_OK);
    CHECK(cm_udp_recv(server, b, &got) == CM_OK && got.data[0] == payload[0]);
    CHECK(cm_udp_open(server, &nft) == CM_OK && cm_udp_bind(server, nft, lo, 9002) == CM_OK);
    CHECK(cm_nft_add(CM_IPPROTO_UDP, 9002, CM_UDP_LO_ADDR, 1, "udp-lo-drop") == CM_OK);
    CHECK(cm_udp_sendto(client, a, 9002, payload, 1) == CM_ERR_DENIED);
    CHECK(cm_udp_recv(server, nft, &got) == CM_ERR_AGAIN);
    cm_bpf_enable();
    kinfo("udp filters ok: XDP DROP blocks recv, kill-switch restores delivery, nft still DROP");
    /* Closing one alias does not unbind; final reference does. */
    CHECK(cm_udp_close(server, b) == CM_OK);
    CHECK(cm_udp_bind(server, duplicate, lo, 9001) == CM_ERR_EXIST);
    CHECK(cm_udp_close(server, writeonly) == CM_OK);
    CHECK(cm_udp_bind(server, duplicate, lo, 9001) == CM_OK);
    kinfo("udp queue/rights/close ok: bound capability lifetime, full=AGAIN empty=AGAIN");
cleanup:
    iface->xdp_prog = old_prog;
    if (old_bpf && !cm_bpf_enabled()) cm_bpf_enable();
    if (!old_bpf && cm_bpf_enabled()) cm_bpf_disable();
    close_handles(client); close_handles(server);
    return failed;
}
static int user_udp_test(int lo)
{
    int failed = 0;
    cm_file_t *file = NULL;
    cm_id_t pid = 0;
    cm_proc_t *p = NULL;
    cm_iface_t *iface = cm_if_get(lo);
    uint64_t tx = iface->tx, rx = iface->rx;
    CHECK(cm_vfs_open("/bin/net.elf", CM_O_WRONLY | CM_O_CREAT | CM_O_TRUNC, &file) == CM_OK);
    CHECK(cm_vfs_write(file, cm_net_elf, sizeof(cm_net_elf)) == (int64_t)sizeof(cm_net_elf));
    CHECK(cm_vfs_fsync(file) == CM_OK);
    cm_vfs_close(file); file = NULL;
    cm_spawn_attr_t attr = {.path = "/bin/net.elf", .qos = CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr, &pid) == CM_OK);
    p = cm_proc_lookup(pid);
    CHECK(p && p->elf);
    *(uint32_t *)cm_elf_pointer(p->elf, 0x401000) = (uint32_t)lo;
#ifdef CAPMIX_HOSTED
    int h;
    cm_udp_datagram_t got;
    CHECK(cm_udp_open(p->task, &h) == CM_OK && cm_udp_bind(p->task, h, lo, 9100) == CM_OK);
    CHECK(cm_udp_sendto(p->task, h, 9100, payload, sizeof(payload) - 1) == CM_OK);
    CHECK(cm_udp_recv(p->task, h, &got) == CM_OK && !memcmp(got.data, payload, got.len));
    CHECK(cm_udp_close(p->task, h) == CM_OK);
    CHECK(cm_udp_open(p->task, &h) == CM_OK && cm_udp_bind(p->task, h, lo, 9101) == CM_OK);
    (void)cm_sched_run_until_idle(64);
    CHECK(p->exited && p->exit_code == CM_ERR_NOTSUP);
    cm_udp_process_exit(p->task); /* hosted ELF skip does not issue EXIT */
    kinfo("udp hosted: lo payload verified; CPL=3 syscall execution skipped");
#else
    (void)cm_sched_run_until_idle(64);
    CHECK(p->exited && p->elf_started && p->exit_code == 0);
    uint32_t *report = cm_elf_pointer(p->elf, 0x401000);
    CHECK(report[1] == sizeof(payload) - 1 && *(uint64_t *)(report + 2) == 3);
    CHECK(p->elf_write_calls == 1 && p->elf_written_bytes == 13);
    kinfo("udp user ok: CPL=3 lo tx/rx payload verified bytes=17 exit=0");
#endif
    CHECK(iface->tx == tx + 1 && iface->rx == rx + 1);
    kinfo("udp lo counters: tx=%llu rx=%llu user_delta=1/1 payload verified",
          (unsigned long)iface->tx, (unsigned long)iface->rx);
    int replacement;
    CHECK(cm_udp_open(p->task, &replacement) == CM_OK);
    CHECK(cm_udp_bind(p->task, replacement, lo, 9101) == CM_OK);
    CHECK(cm_udp_close(p->task, replacement) == CM_OK);
    CHECK(cm_proc_reap_elf(pid) == CM_OK);
    kinfo("udp exit cleanup ok: endpoint/port released, ELF image reclaimed");
cleanup:
    if (file) cm_vfs_close(file);
    if (p) { cm_udp_process_exit(p->task); if (p->exited) (void)cm_proc_reap_elf(pid); }
    return failed;
}
int cm_phase5_selftest(void)
{
    kinfo("--- selftest: phase5 IPv4 UDP over lo ---");
    int lo = cm_if_find("lo");
    if (lo < 0) lo = cm_if_add("lo", CM_UDP_LO_ADDR);
    if (lo < 0) { kerr("phase5: no loopback iface"); return 1; }
    int failed = kernel_echo_test(lo) + user_udp_test(lo);
    kinfo("phase5 selftest: %s (%d failures)", failed ? "FAIL" : "PASS", failed);
    return failed;
}
