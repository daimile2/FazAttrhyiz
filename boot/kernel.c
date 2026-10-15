#include "capmix/e1000.h"
#include "capmix/ahci.h"
#include "capmix/power.h"
#include "capmix/boot.h"
#include "capmix/shell.h"
#include "capmix/virtio_net.h"
#include "capmix/console.h"
#include "capmix/persist.h"
#include "capmix/kernel.h"
#include "capmix/numa.h"
#include "capmix/fs.h"
#include "capmix/io.h"
#include "capmix/security.h"
#include "capmix/compress.h"
#include "capmix/arch.h"

#include <string.h>

static volatile int g_sw_seq;
static void switch_a(void *arg)
{
    CM_UNUSED(arg);
    g_sw_seq += 1;
    cm_arch_ctx_swap(0, 1);
    g_sw_seq += 10;
}
static void switch_b(void *arg)
{
    CM_UNUSED(arg);
    g_sw_seq += 100;
    cm_arch_ctx_swap(1, 0);
}

static int g_ping_count;
static int g_pong_count;
static int g_replies;
static cm_task_t *g_server_task;
static cm_task_t *g_client_task;
static int g_srv_end, g_cli_end;
static int g_cli_phase; /* 0 = send, 1 = recv, 2 = done */
static int g_srv_done;

static void server_fn(void *arg)
{
    CM_UNUSED(arg);
    if (g_srv_done) return;
    cm_ipc_msg_t m;
    cm_error_t e = cm_ipc_recv(g_server_task, g_srv_end, &m);
    if (e == CM_OK) {
        g_pong_count++;
        m.opcode = 0xBEEF;
        m.trace = cm_trace_child(m.trace);
        cm_ipc_send(g_server_task, g_srv_end, &m);
        g_srv_done = 1;
        cm_thread_t *cur = cm_sched_current();
        if (cur) {
            cur->state = CM_TH_DEAD;
            /* leave the runqueue; tick will not re-queue a DEAD thread
             * because state != RUNNING after we mark DEAD. */
        }
    }
}

static void client_fn(void *arg)
{
    CM_UNUSED(arg);
    cm_thread_t *cur = cm_sched_current();
    if (g_cli_phase == 0) {
        cm_ipc_msg_t req;
        memset(&req, 0, sizeof(req));
        req.trace = cm_trace_new();
        req.opcode = 0xCAFE;
        req.len = 4;
        req.data[0] = 42;
        if (cm_ipc_send(g_client_task, g_cli_end, &req) == CM_OK)
            g_ping_count++;
        g_cli_phase = 1;
        return;
    }
    if (g_cli_phase == 1) {
        cm_ipc_msg_t reply;
        if (cm_ipc_recv(g_client_task, g_cli_end, &reply) == CM_OK) {
            g_replies++;
            g_cli_phase = 2;
            if (cur) cur->state = CM_TH_DEAD;
        }
    }
}

static void worker_fn(void *arg)
{
    int *n = (int *)arg;
    if (n) (*n)++;
    if (n && *n > 200) {
        cm_thread_t *cur = cm_sched_current();
        if (cur) cur->state = CM_TH_DEAD;
    }
}

/* --- preemption proof -------------------------------------------------
 * Two CPU-bound threads that never yield. Nothing but a timer tick can
 * take the CPU away from them, so preempts>0 is the proof.
 *
 * Termination must NOT depend on the timer: the previous round hung
 * because the backstop waited for cm_timer_ticks() to advance, and that
 * is exactly the thing under test. Each hog now yields after a fixed
 * iteration count, so the test always returns in tens of milliseconds
 * and reports WHY preemption did or did not happen.
 */
#define CM_SPIN_QUANTUM 10000000   /* ~3 ms at 3.2 GHz; spans >= 1 tick */

static volatile int  g_spin_a_n;
static volatile int  g_spin_b_n;
static cm_thread_t  *g_spin_a;
static cm_thread_t  *g_spin_b;
static uint64_t      g_spin_a_rt;
static uint64_t      g_spin_b_rt;
static int           g_spin_if_at_entry;

static void spin_fn(void *arg)
{
    volatile int *n = (volatile int *)arg;
    for (;;) {
        (*n)++;

        /* One-shot: does the very first thread context run with IRQs on?
         * cm_arch_ctx_swap restores the target's saved RFLAGS, so a fresh
         * thread inherits whatever the scheduler had. If this prints 0 the
         * timer can never interrupt a thread and preemption is impossible. */
        if (*n == 1 && !g_spin_if_at_entry) {
            uint64_t rf;
            __asm__ volatile("pushfq; popq %0" : "=r"(rf));
            g_spin_if_at_entry = (int)((rf >> 9) & 1);
            kinfo("preempt: hog entry IF=%d (must be 1 for any tick to land)",
                  g_spin_if_at_entry);
        }

        if (*n >= CM_SPIN_QUANTUM) {
            *n = 0;
            cm_sched_yield();
        }
    }
}

static void preempt_selftest(int *fails)
{
    kinfo("--- selftest: preemption (two CPU hogs, 1 ms tick) ---");
    /* Highest empty band: guarantees cm_eevdf_pick() actually chooses the
     * hogs instead of the leftover Utility/Background/Idle threads. */
    cm_task_t *ta = cm_task_create("hog-a", CM_QOS_USER_INTERACTIVE);
    cm_task_t *tb = cm_task_create("hog-b", CM_QOS_USER_INTERACTIVE);
    if (!ta || !tb) { kerr("hog task create"); (*fails)++; return; }

    g_spin_a_n = 0;  g_spin_b_n = 0;
    g_spin_a_rt = 0; g_spin_b_rt = 0;
    g_spin_if_at_entry = 0;
    g_spin_a = cm_thread_create(ta, spin_fn, (void *)&g_spin_a_n, CM_QOS_USER_INTERACTIVE);
    g_spin_b = cm_thread_create(tb, spin_fn, (void *)&g_spin_b_n, CM_QOS_USER_INTERACTIVE);
    if (!g_spin_a || !g_spin_b) { kerr("hog thread create"); (*fails)++; return; }

    uint64_t p0 = cm_sched_preempts();
    uint64_t t0 = cm_timer_ticks();
    int steps = cm_sched_run_until_idle(16);

    /* Snapshot BEFORE anything can reap the hogs: cm_thread_reap() memsets
     * the whole struct, which is what zeroed run_ticks last round. */
    g_spin_a_rt = g_spin_a->run_ticks;
    g_spin_b_rt = g_spin_b->run_ticks;
    uint64_t p1 = cm_sched_preempts();
    uint64_t t1 = cm_timer_ticks();
    uint64_t rf = 0;
    __asm__ volatile("pushfq; popq %0" : "=r"(rf));
    int if_after = (int)((rf >> 9) & 1);

    kinfo("preempt: steps=%d preempts=%llu ticks=%llu -> %llu",
          steps, (unsigned long)(p1 - p0), (unsigned long)t0, (unsigned long)t1);
    kinfo("preempt: hog-a n=%d run_ticks=%llu", g_spin_a_n, (unsigned long)g_spin_a_rt);
    kinfo("preempt: hog-b n=%d run_ticks=%llu", g_spin_b_n, (unsigned long)g_spin_b_rt);
    kinfo("preempt: IF after test=%d (want 1)", if_after);

    if (g_spin_a_rt == 0 || g_spin_b_rt == 0) { kerr("one hog never ran"); (*fails)++; }
        if (!if_after) { kerr("IF left clear - scheduler runs with IRQs off"); (*fails)++; }

    #ifndef CAPMIX_HOSTED
        /* Preemption is driven by cm_timer_isr() -> cm_sched_preempt_now().
         * Hosted has no LAPIC and no timer, so g_preempt_count stays 0 there by
         * construction. Asserting it on hosted would fail every run and say
         * nothing: the cooperative round-robin above (8/8 run_ticks) is all
         * hosted CAN prove, and that is what the two asserts above check. */
        if (p1 == p0) { kerr("no preemption - hogs were never interrupted"); (*fails)++; }
        if (p1 > p0 && g_spin_a_rt && g_spin_b_rt && if_after)
            kinfo("preempt: OK preempts=%llu a=%llu b=%llu IF=1",

                (unsigned long)(p1 - p0),
                (unsigned long)g_spin_a_rt, (unsigned long)g_spin_b_rt);
    #else
        kinfo("preempt: cooperative round-robin OK (a=%llu b=%llu) — "

            "preempt count not asserted: hosted has no timer ISR",
            (unsigned long)g_spin_a_rt, (unsigned long)g_spin_b_rt);
    #endif
    g_spin_a->state = CM_TH_DEAD; cm_thread_reap(g_spin_a);
    g_spin_b->state = CM_TH_DEAD; cm_thread_reap(g_spin_b);
}



static int test_lsm(uint32_t op, cm_id_t subject, uint64_t obj)
{
    CM_UNUSED(subject);
    CM_UNUSED(obj);
    return op == CM_OP_TASK_FOR_PID ? -1 : 0;
}

void capmix_banner(void)
{
    kinfo("====================================================");
    kinfo("%s", CAPMIX_BANNER);
    kinfo("skeleton: capability hybrid (Zircon + L4 IPC + XNU split)");
    kinfo("mm: no-overcommit + buddy/SLUB + 4-level pressure");
    kinfo("sched: EEVDF + QoS bands + turnstile");
    kinfo("obs: trace_id on every IPC (day-0, not bolted on)");
    kinfo("proc: posix_spawn first-class, fork is compat-only");
    kinfo("fs: COW volume (APFS + Btrfs csum + XFS AG + ReFS scrub)");
    kinfo("io: irq split + uring + blk-mq + user-mode drivers");
    kinfo("net: in-kernel + XDP/eBPF (kill-switch) + nft");
    kinfo("sec: cap + Seatbelt + SIP + one-shot authz + signing");
    kinfo("virt: Type-1 KVM + job/cgroup v2");
    kinfo("obs: ETW session + hist + cap-gated consume");
    kinfo("====================================================");
}

void capmix_init(const cm_mem_region_t *regions, int nregions)
{
    cm_arch_early_init();
    cm_klog_init();
    capmix_banner();
    cm_obs_init();
    cm_obs_emit(cm_trace_new(), CM_EV_BOOT, 0, 0, 0);
    cm_mm_init(regions, nregions);
    {
        uint64_t np = 32;
        cm_paddr_t base = 0;
        if (regions && nregions > 0) {
            base = regions[0].base;
            np = regions[0].npages;
        }
        (void)cm_paging_init(base, np ? np : 32);
    }
#ifdef CAPMIX_HOSTED
    capmix_after_paging();
#endif
}

void capmix_after_paging(void)
{
    cm_console_init();
    cm_numa_init();
    cm_power_init();
    cm_cap_init();
    cm_sched_init();
    cm_proc_init();
    cm_syscall_init();
    cm_io_init();      /* blk first: the VFS mounts the COW volume from it */
    cm_error_t root_mount = cm_vfs_init();
    cm_product_record_root(root_mount);
    if (cm_boot_is_selftest()) cm_m3_prepare(root_mount);
    cm_net_init();
    cm_security_init();
    cm_virt_init();
    cm_lapic_init();
    {
        
        uint64_t sp;
        __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
        kinfo("probe rsp=%llx (boot_stack=%p) after_paging",
              (unsigned long)sp, cm_boot_stack_base());
    }
    cm_smp_probe();
    cm_numa_update_cpus();
        cm_smp_start();
}

static int g_selftest_fails;

static void probe(const char *tag)
{
    kinfo("probe: %s firings=%llu ticks=%llu",
          tag,
          (unsigned long)cm_idt_firings(),
          (unsigned long)cm_timer_ticks());
}

int capmix_selftest(void)
{
    {
        uint64_t sp;
        __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
        kinfo("probe rsp=%llx (boot_stack=%p) selftest",
              (unsigned long)sp, cm_boot_stack_base());
    }
    kinfo("smp: selftest on cpu %d of %d", cm_cpu_id(), cm_cpu_count());
    kinfo("checkpoint A");
    int fails = cm_console_selftest();
    fails += cm_shell_selftest();
    fails += cm_power_selftest();
    fails += cm_ahci_selftest();
    extern int cm_disk_layout_selftest(void);
    fails += cm_disk_layout_selftest();
    fails += cm_timer_calibration_selftest();
    fails += cm_buddy_smp_selftest();
    {
        uint64_t t0 = cm_rdtsc();
        for (int i = 0; i < 1000; i++)
            (void)cm_rdtsc();
        uint64_t t1 = cm_rdtsc();
        kinfo("rdtsc: %llu cycles/call", (unsigned long)((t1 - t0) / 1000));
        kinfo("checkpoint B");
        t0 = cm_rdtsc();
        for (int i = 0; i < 1000; i++)
            cm_obs_emit(1, CM_EV_BOOT, 0, 0, 0);
        t1 = cm_rdtsc();
        kinfo("obs_emit: %llu cycles/call", (unsigned long)((t1 - t0) / 1000));
        kinfo("checkpoint C");
    }
    kinfo("checkpoint D");
    {
        uint64_t t0 = cm_timer_ticks();
        for (volatile int i = 0; i < 2000000; i++) {
            if ((i % 200000) == 0) {
                uint64_t tsc = cm_rdtsc();
                uint64_t dl = cm_tsc_deadline();
                kinfo("timer-spin i=%d firings=%llu ticks=%llu ccr=%x tsc=%llx dl=%llx d=%lld",
                      i,
                      (unsigned long)cm_idt_firings(),
                      (unsigned long)cm_timer_ticks(),
                      (unsigned)cm_lapic_ccr(),
                      (unsigned long)tsc, (unsigned long)dl,
                      (long)(dl - tsc));
            }
        }
        uint64_t t1 = cm_timer_ticks();
        kinfo("timer: ticks %llu -> %llu", (unsigned long)t0, (unsigned long)t1);
        if (t1 <= t0)
            kwarn("timer: no tick yet (TSC-deadline may be unsupported)");
        probe("after-timer-spin");
        kinfo("checkpoint E");
    }
    kinfo("--- selftest: wkdm roundtrip + configurable page walk ---");
    extern int cm_phase0_selftest(void);
    if (cm_phase0_selftest() != 0) fails++;
    extern int cm_numa_selftest(void);
    fails += cm_numa_selftest();
    if (cm_wkdm_selftest() != 0) fails++;
    if (cm_paging_tables() == 0) { kerr("no page tables built"); fails++; }
    else cm_paging_dump();
    {
        static uint8_t stka[16384], stkb[16384];
        g_sw_seq = 0;
        if (cm_arch_ctx_init(0, switch_a, NULL, stka, sizeof(stka)) != 0 ||
            cm_arch_ctx_init(1, switch_b, NULL, stkb, sizeof(stkb)) != 0) {
            kerr("ctx init failed — not skipped");
            fails++;
        } else {
            cm_arch_ctx_swap(-1, 0);
            kinfo("ctx: fiber switch seq=%d (want 111)", g_sw_seq);
            if (g_sw_seq != 111) {
                kerr("stack switch did not run both sides");
                fails++;
            }
        }
    }

    {
        int rec = 0;
        cm_task_t *rt = cm_task_create("reap", CM_QOS_UTILITY);
        for (int i = 0; i < 40; i++) {
            cm_thread_t *th = cm_thread_create(rt, worker_fn, NULL, CM_QOS_UTILITY);
            if (!th) { kerr("thread slot exhausted at %d", i); fails++; break; }
            th->state = CM_TH_DEAD;
            cm_thread_reap(th);
            rec++;
        }
        cm_thread_t *again = cm_thread_create(rt, worker_fn, NULL, CM_QOS_UTILITY);
        if (!again) { kerr("slot not recycled after reap"); fails++; }
        else {
            again->state = CM_TH_DEAD;
            cm_thread_reap(again);
            kinfo("sched: reaped %d threads, slot recycled", rec);
        }
    }

    kinfo("--- selftest: buddy / commit ---");
    void *p0 = cm_page_alloc(0);
    void *p3 = cm_page_alloc(3);
    if (!p0 || !p3) { kerr("page alloc failed"); fails++; }
    cm_page_free(p0, 0);
    cm_page_free(p3, 3);

    if (cm_commit_charge(64) != CM_OK) { kerr("commit 64 failed"); fails++; }
    cm_commit_uncharge(64);

    /* Exhaust commit limit on purpose. */
    cm_mm_stats_t st;
    cm_mm_stats(&st);
    if (cm_commit_charge(st.commit_limit + 1) != CM_ERR_OVERCOMMIT) {
        kerr("no-overcommit did not trip");
        fails++;
    } else {
        kinfo("no-overcommit correctly refused over-limit charge");
    }
    cm_mm_dump();
    probe("after-buddy");
    cm_pressure_tick();

    kinfo("--- selftest: capabilities + channel IPC ---");
    g_server_task = cm_task_create("ioserver", CM_QOS_USER_INTERACTIVE);
    g_client_task = cm_task_create("app", CM_QOS_USER_INITIATED);
    if (!g_server_task || !g_client_task) { kerr("task create"); fails++; }

    int a = -1, b = -1;
    if (cm_channel_create(g_server_task, &a, &b) != CM_OK) {
        kerr("channel create");
        fails++;
    } else {
        /* Transfer endpoint B to the client (capability transfer). */
        int cli_h = -1;
        if (cm_handle_duplicate(g_server_task, b, g_client_task, &cli_h, CM_RIGHT_ALL) != CM_OK) {
            kerr("handle transfer");
            fails++;
        }
        cm_handle_drop(g_server_task, b);
        g_srv_end = a;
        g_cli_end = cli_h;
    }

    cm_thread_create(g_client_task, client_fn, NULL, CM_QOS_USER_INITIATED);
    cm_thread_create(g_server_task, server_fn, NULL, CM_QOS_USER_INTERACTIVE);

    int bg_work = 0;
    cm_task_t *bg = cm_task_create("backgroundd", CM_QOS_BACKGROUND);
    cm_thread_create(bg, worker_fn, &bg_work, CM_QOS_BACKGROUND);

    int steps = cm_sched_run_until_idle(32);
    kinfo("sched ran %d steps, ping=%d pong=%d replies=%d bg_work=%d",
          steps, g_ping_count, g_pong_count, g_replies, bg_work);
    probe("after-ipc");
    if (g_ping_count == 0) { kerr("client never sent"); fails++; }
    if (g_pong_count == 0) { kerr("server never received"); fails++; }

    extern int cm_phase1_selftest(void);
    fails += cm_phase1_selftest();

    kinfo("--- selftest: posix_spawn + first user process ---");
    {
        cm_id_t child = 0;
        cm_spawn_attr_t attr = {
            .path = "/bin/hello",
            .entry = cm_user_hello,
            .qos = CM_QOS_USER_INITIATED,
            .image_pages = CM_USER_IMAGE_PAGES,
            .stack_pages = CM_USER_STACK_PAGES,
        };
        if (cm_posix_spawn(&attr, &child) != CM_OK) {
            kerr("posix_spawn failed");
            fails++;
        } else {
            int u_steps = cm_sched_run_until_idle(16);
            cm_proc_t *hp = cm_proc_lookup(child);
            kinfo("user hello: pid=%llu steps=%d ran=%d exited=%d code=%lld",
                  (unsigned long)child, u_steps, cm_user_hello_ran(),
                  hp ? hp->exited : -1, (long)(hp ? hp->exit_code : -1));
            if (!cm_user_hello_ran() || !hp || !hp->exited) {
                kerr("hello world did not run to exit");
                fails++;
            }
        }
        cm_id_t dummy = 1;
        if (cm_fork_compat(&dummy) != CM_ERR_NOTSUP) {
            kerr("fork should be compat-denied");
            fails++;
        } else {
            kinfo("fork correctly refused (conflict #2)");
        }
    }

    kinfo("--- selftest: phase 3 (pressure pipeline + VFS + COW volume) ---");
    {
        extern int cm_phase3_selftest(void);
        fails += cm_phase3_selftest();
        cm_mm_dump();
    }

    extern int cm_phase2_selftest(void);
    fails += cm_phase2_selftest();
    extern int cm_p1a_selftest(void);
    fails += cm_p1a_selftest();

    fails += cm_m4_selftest();
    fails += cm_m8_selftest();
    extern int cm_phase5_selftest(void);
    fails += cm_phase5_selftest();
    extern int cm_p1c_selftest(void);
    fails += cm_p1c_selftest();
    extern int cm_p1b_selftest(void);
    fails += cm_p1b_selftest();
    extern int cm_p1d_selftest(void);
    fails += cm_p1d_selftest();
    extern int cm_tcp_elf_selftest(void);
    fails += cm_tcp_elf_selftest();
    extern int cm_argv_selftest(void);
    fails += cm_argv_selftest();
    fails += cm_sched_claim_selftest();
    fails += cm_linux_selftest();
    extern int cm_concurrent_selftest(void);
    fails += cm_concurrent_selftest();
    extern int cm_control_selftest(void);
    fails += cm_control_selftest();

    kinfo("--- selftest: irq + blk-mq + user driver isolation ---");
    {
        static uint8_t wbuf[CM_BLK_SECTOR];
        static uint8_t rbuf[CM_BLK_SECTOR];
        memset(wbuf, 0xA5, sizeof(wbuf));
        memset(rbuf, 0, sizeof(rbuf));

        cm_sqe_t sq;
        cm_cqe_t cq;
        memset(&sq, 0, sizeof(sq));
        sq.opcode = CM_OP_WRITE;
        sq.addr = (uint64_t)(uintptr_t)wbuf;
        sq.len = CM_BLK_SECTOR;
        sq.off = 0;
        sq.user_data = 0x11;
        if (cm_uring_submit(&sq) != CM_OK) { kerr("uring write submit"); fails++; }
        if (cm_uring_complete(&cq) != CM_OK || cq.res < 0) {
            kerr("uring write cq"); fails++;
        }

        sq.opcode = CM_OP_READ;
        sq.addr = (uint64_t)(uintptr_t)rbuf;
        sq.user_data = 0x22;
        if (cm_uring_submit(&sq) != CM_OK) { kerr("uring read submit"); fails++; }
        if (cm_uring_complete(&cq) != CM_OK || cq.res < 0) {
            kerr("uring read cq"); fails++;
        }
        if (memcmp(wbuf, rbuf, CM_BLK_SECTOR) != 0) {
            kerr("blk-mq ramdisk mismatch");
            fails++;
        } else {
            kinfo("blk-mq: write/read 512B via uring ok, inflight=%u completed=%llu",
                  cm_uring_inflight(), (unsigned long)cm_blk_completed());
        }

        /* Trace ELF uses the live ramdisk before phase4 deliberately offlines it. */
        extern int cm_phase8_selftest(void);
        fails += cm_phase8_selftest();
        extern int cm_phase4_selftest(void);
        fails += cm_phase4_selftest();
    }

    kinfo("--- selftest: net + XDP/eBPF + nft ---");
    {
        int lo = cm_if_find("lo");
        if (lo < 0) lo = cm_if_add("lo", 0x7f000001);
        int eth = cm_if_add("eth0", 0x0a000002);
        CM_UNUSED(lo);

        /* Verifier must reject a backward jump. */
        cm_bpf_ins_t bad[] = {
            { CM_BPF_JEQ, 0xFF, 0 },
            { CM_BPF_RET, 0, CM_XDP_PASS },
        };
        if (cm_bpf_load("evil-loop", bad, 2) >= 0) {
            kerr("verifier let a backward jump through");
            fails++;
        } else {
            kinfo("ebpf: verifier rejected looping program");
        }

        /* Drop UDP/53 at XDP. */
        cm_bpf_ins_t drop53[] = {
            { CM_BPF_LD_PROTO, 0, 0 },
            { CM_BPF_JNE, 2, CM_IPPROTO_UDP },
            { CM_BPF_LD_DPORT, 0, 0 },
            { CM_BPF_JEQ, 1, 53 },
            { CM_BPF_RET, 0, CM_XDP_PASS },
            { CM_BPF_RET, 0, CM_XDP_DROP },
        };
        int pid = cm_bpf_load("xdp-drop-dns", drop53, 6);
        if (pid < 0) { kerr("good xdp rejected"); fails++; }
        else if (cm_bpf_attach(pid, eth) != CM_OK) { kerr("attach"); fails++; }

        uint8_t pkt[64];
        memset(pkt, 0, sizeof(pkt));
        pkt[12] = 0x08; pkt[13] = 0x00;           /* IPv4 */
        pkt[14] = 0x45;                            /* ver/ihl */
        pkt[23] = CM_IPPROTO_TCP;                  /* proto */
        pkt[26] = 0x0a; pkt[29] = 0x01;            /* saddr 10.0.0.1 */
        pkt[36] = 0x00; pkt[37] = 80;              /* tcp dport 80 */
        cm_xdp_act_t a_tcp = cm_net_rx(eth, pkt, 54);
        pkt[23] = CM_IPPROTO_UDP;
        pkt[36] = 0; pkt[37] = 53;                 /* udp dport 53 */
        cm_xdp_act_t a_dns = cm_net_rx(eth, pkt, 42);
        kinfo("xdp: tcp/80 → %d (want PASS=%d), udp/53 → %d (want DROP=%d)",
              (int)a_tcp, CM_XDP_PASS, (int)a_dns, CM_XDP_DROP);
        if (a_tcp != CM_XDP_PASS || a_dns != CM_XDP_DROP) {
            kerr("xdp classification wrong");
            fails++;
        }

        cm_bpf_disable();
        cm_xdp_act_t a_off = cm_net_rx(eth, pkt, 42);
        kinfo("xdp off: udp/53 → %d (must PASS once kill-switch flipped)", (int)a_off);
        if (a_off != CM_XDP_PASS) {
            kerr("kill-switch did not bypass XDP");
            fails++;
        }
        cm_bpf_enable();

        cm_nft_add(CM_IPPROTO_ICMP, 0, 0x0a000001, 1, "drop-icmp-from-10.0.0.1");
        pkt[23] = CM_IPPROTO_ICMP;
        pkt[34] = 0; pkt[35] = 0;
        /* saddr already 10.0.0.1 */
        cm_xdp_act_t a_icmp = cm_net_rx(eth, pkt, 42);
        kinfo("nft: icmp from 10.0.0.1 → %d (want DROP)", (int)a_icmp);
        if (a_icmp != CM_XDP_DROP) { kerr("nft miss"); fails++; }

        cm_net_dump();
        if (cm_bpf_rejects() == 0) { kerr("no verifier rejects recorded"); fails++; }
    }

    kinfo("--- selftest: capability + seatbelt + SIP + authz + sign ---");
    {
        cm_task_t *sbox = cm_task_create("sandboxed", CM_QOS_DEFAULT);
        cm_object_t *vmo = cm_obj_mint(CM_OBJ_VMO, NULL, CM_RIGHT_READ | CM_RIGHT_INSPECT);
        int hid = cm_handle_install(sbox, vmo, CM_RIGHT_READ | CM_RIGHT_INSPECT);
        if (cm_cap_check(sbox, hid, CM_RIGHT_WRITE) != CM_ERR_DENIED) {
            kerr("cap should deny write"); fails++;
        } else {
            kinfo("cap: write denied without the bit (deny-by-default)");
        }
        if (cm_cap_check(sbox, hid, CM_RIGHT_READ) != CM_OK) {
            kerr("cap read should pass"); fails++;
        }

        cm_seatbelt_t sb = {
            .allow_mask = CM_OP_FILE_READ | CM_OP_NET_CONNECT,
            .deny_mask  = CM_OP_FILE_WRITE | CM_OP_KEXT_LOAD,
            .label = "app.sbpl",
        };
        cm_seatbelt_apply(sbox->id, &sb);
        if (cm_seatbelt_check(sbox->id, CM_OP_FILE_WRITE) != CM_ERR_DENIED) {
            kerr("seatbelt write"); fails++;
        } else {
            kinfo("seatbelt: file-write blocked per-operation");
        }
        if (cm_seatbelt_check(sbox->id, CM_OP_FILE_READ) != CM_OK) {
            kerr("seatbelt read"); fails++;
        }

        if (cm_sip_write_check(0xffffffff80001000ull) != CM_ERR_DENIED) {
            kerr("SIP should stop kernel-text poke"); fails++;
        } else {
            kinfo("sip: root cannot write kernel text");
        }

        extern int cm_phase6_selftest(void);
        fails += cm_phase6_selftest();

        /* One-shot elevate for kext load — second consume fails. */
        if (cm_authz_grant(sbox->id, CM_OP_KEXT_LOAD) < 0) { kerr("authz grant"); fails++; }
        if (cm_authz_consume(sbox->id, CM_OP_KEXT_LOAD) != CM_OK) { kerr("authz first"); fails++; }
        if (cm_authz_consume(sbox->id, CM_OP_KEXT_LOAD) != CM_ERR_DENIED) {
            kerr("authz must die after one use"); fails++;
        } else {
            kinfo("authz: second kext-load refused (not a UAC process token)");
        }

        cm_lsm_register(test_lsm);
        if (cm_lsm_check(CM_OP_TASK_FOR_PID, sbox->id, 0) != CM_ERR_DENIED) {
            kerr("lsm tfp"); fails++;
        } else {
            kinfo("lsm: task_for_pid extra-denied (optional MAC)");
        }
        cm_lsm_register(NULL);
        cm_mitigations_dump();
    }

    extern int cm_phase7_selftest(void);
    fails += cm_phase7_selftest();
    extern int cm_phase9_selftest(void);
    fails += cm_phase9_selftest();
    kinfo("--- selftest: job/cgroup + KVM ---");
    {
        cm_job_t *app = cm_job_create("app.bundle", CM_NS_ALL);
        cm_job_t *svc = cm_job_create("system.slice", CM_NS_PID | CM_NS_MNT | CM_NS_CGROUP);
        if (!app || !svc) { kerr("job create"); fails++; }
        app->cg.memory_max = 8;
        if (cm_cg_charge(app, 4) != CM_OK) { kerr("cg charge 4"); fails++; }
        if (cm_cg_charge(app, 8) != CM_ERR_OVERCOMMIT) {
            kerr("cgroup memory.max should trip"); fails++;
        } else {
            kinfo("cgroup v2: memory.max enforced on job");
        }

        cm_id_t p_a = 0, p_b = 0;
        cm_spawn_attr_t ja = {
            .path = "helper-a", .entry = cm_user_hello,
            .qos = CM_QOS_UTILITY, .image_pages = 2, .stack_pages = 1
        };
        cm_spawn_attr_t jb = {
            .path = "helper-b", .entry = cm_user_hello,
            .qos = CM_QOS_UTILITY, .image_pages = 2, .stack_pages = 1
        };
        (void)cm_posix_spawn(&ja, &p_a);
        (void)cm_posix_spawn(&jb, &p_b);
        cm_job_insert(app, p_a);
        cm_job_insert(svc, p_b);
        cm_job_kill(app);
        if (!svc->killed && cm_proc_lookup(p_b) && !cm_proc_lookup(p_b)->exited)
            kinfo("coalition: killing app.bundle left system.slice helper alive");
        else if (svc->killed) {
            kerr("job kill leaked"); fails++;
        } else {
            kinfo("coalition: system.slice job still live");
        }

        cm_vm_t *vm = cm_vm_create("guest0", svc, 16, 2);
        if (!vm) { kerr("vm create"); fails++; }
        else {
            if (cm_vcpu_run(vm, 0) != CM_OK) { kerr("vcpu"); fails++; }
            if (cm_vfio_attach(vm, 0x000100) != CM_OK) { kerr("vfio"); fails++; }
            kinfo("kvm: vmexits=%llu vfio=%d continuous_t=%llu",
                  (unsigned long)vm->vmexits, vm->nvfio,
                  (unsigned long)cm_continuous_time());
        }
        cm_virt_dump();
    }

    kinfo("--- selftest: observability as capability ---");
    {
        int sess = cm_obs_open_session(0xFFu, 42);
        if (sess < 0) { kerr("session"); fails++; }
        if (cm_obs_authorize(99, sess) != CM_ERR_DENIED) {
            kerr("obs cap should deny stranger"); fails++;
        } else {
            kinfo("obs: session consume is a capability, not a global switch");
        }
        if (cm_obs_authorize(42, sess) != CM_OK) { kerr("obs owner"); fails++; }

        /* Observation is a capability: OFF by default (one emit is ~447
         * cycles and an IPC round trip emits ~5 times), paid only while an
         * authorized session is consuming. Turn it on for the assertions,
         * then off again — otherwise this block silently verifies nothing
         * (events=0) and still reports PASS. */
        cm_obs_enable(1);
        uint64_t ev_before = cm_obs_count();
        CM_TRACEPOINT(CM_EV_SYS, 8, 0, 0);
        CM_TRACEPOINT(CM_EV_FS, 1, 2, 3);
        if (cm_obs_count() <= ev_before) {
            kerr("obs: no events recorded while enabled"); fails++;
        }
        uint64_t seen = cm_obs_session_seen(sess);
        if (seen == 0) { kerr("obs: authorized session saw nothing"); fails++; }
        cm_obs_enable(0);
        cm_hist_t h;
        cm_hist_init(&h, 8);
        uint64_t rtt_sample = cm_ipc_bench_rtt(64);
        cm_hist_add(&h, rtt_sample);
        cm_hist_add(&h, 3);
        cm_hist_add(&h, 40);
        cm_hist_add(&h, 400);
        cm_hist_dump("ipc_rtt_cycles", &h);
        if (h.total != 4) { kerr("hist"); fails++; }
        kinfo("obs events=%llu session_seen=%llu",
              (unsigned long)cm_obs_count(),
              (unsigned long)cm_obs_session_seen(sess));
    }

    kinfo("--- selftest: end-to-end syscall cost (ring3 floor) ---");
    {
        extern uint64_t cm_bench_syscall_nop(int rounds);
        extern uint64_t cm_bench_syscall_ipc(int rounds);

        uint64_t nop = cm_bench_syscall_nop(1000);
        kinfo("e2e: syscall NOP = %llu cycles/call (dispatch only)",
              (unsigned long)nop);

        uint64_t ipc = cm_bench_syscall_ipc(200);
        if (ipc)
            kinfo("e2e: syscall IPC = %llu cycles/rtt", (unsigned long)ipc);
        else
            kwarn("e2e: IPC bench returned 0 (not measurable here)");
    }

    kinfo("--- selftest: IPC RTT (memcpy baseline + cross-stack) ---");
    uint64_t rtt = cm_ipc_bench_rtt(1000);
    if (rtt == 0) { kerr("rtt bench failed"); fails++; }
    uint64_t rtts = cm_ipc_bench_rtt_switched(64);
    if (rtts == 0) { kerr("cross-stack rtt produced no completions"); fails++; }
    probe("after-rtt");

#ifndef CAPMIX_HOSTED
    g_selftest_fails = fails;
    
    kinfo("--- selftest: ring3 + syscall + per-process PML4 ---");
    cm_usermode_selftest(); /* does not return: land_user → finish → hlt */
#endif

    g_selftest_fails = fails;
    cm_selftest_finish();
    return g_selftest_fails;
}

void cm_selftest_finish(void)
{
    int fails = g_selftest_fails;
    probe("finish");
        preempt_selftest(&fails);
#if 1  /* re-enabled: per-CPU rsp0 landed, hunting the race now */
            cm_smp_sched_stress(2000);
#endif

            cm_smp_dump();
    cm_cap_dump();
    cm_sched_dump();
    cm_ipc_dump();
    cm_obs_dump(12);
    cm_mitigations_dump();
    fails += cm_m3_finish();
    kinfo("--- selftest result: %s (%d failures) ---",
          fails ? "FAIL" : "PASS", fails);
#ifndef CAPMIX_HOSTED
    kinfo("freestanding selftest rc=%d — hlt", fails);
    cm_console_idle();
#endif
}
