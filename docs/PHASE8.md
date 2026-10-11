# Phase 8: static tracepoints and capability reads

`include/capmix/trace.h` declares the compile-time catalog: sched switch/enqueue/
dequeue, anonymous soft fault, filesystem COW block split, channel send/receive,
UDP tx/rx, deferred IRQ delivery and sign accept/reject. Every entry has a stable
id, name, category and mask bit. `CM_TRACE`/`TRACE_SCHED_SWITCH`/`TRACE_IPC_SEND`
check the enabled mask before evaluating arguments. `CM_TP_BUILD_MASK` clips
points at compile time; the default compiles all twelve. Default capture is off.
No kprobe, runtime attach or core subsystem policy changes are introduced.

New records contain TSC, CPU, task id (0 for boot/no current task), thread id,
trace id, parent, per-CPU sequence, TP id and three numeric arguments. Each CPU
has 512 slots. The local writer disables maskable interrupts/preemption only
while selecting CPU/context and publishing the event. There is no writer
spinlock and no allocation. This API is not intended for NMI handlers. Each
slot has an atomic in-progress/complete stamp and atomic payload words. Readers
snapshot a published head and verify the stamp before/after copying; racing
writers yield AGAIN rather than a torn record. Full rings overwrite old slots;
read cursors report how many records were overwritten. No cross-CPU TSC sorting
or lossless transport is claimed.

Legacy obs storage is also per-CPU, with atomic stamped snapshots and real CPU
IDs/TSC. Kernel-only compatibility enable/provider/session APIs remain for old
selftests. They are not user authorization interfaces. New user reads use TRACE
capabilities exclusively.

Root trace IDs use an atomic sequence in the high 48 bits and hop=1 in the low
16 bits. `cm_trace_child` preserves the root and advances the linear hop (at
hop exhaustion it starts a new root). Parent is the preceding hop, not an
independent DAG identity. Channel send/receive carry the exact message ID.
A running receiving thread stores that chain as observational context; sched,
FS and other points with no explicit id inherit it. CPU-local sequence gives
publication order. This does not claim complete distributed causal propagation
or uniqueness among sibling branches with equal hops.

Trusted kernel policy calls `cm_trace_open(reader, allowed_mask, subject_task,
cpu_mask)` to mint a `CM_OBJ_TRACE` handle. Subject 0 is an explicitly granted
all-task scope. No unrestricted user open syscall exists. Allowed scope is
immutable; WRITE permits activation of a subset. Active sessions determine the
union capture mask, while the writer checks interested session scopes and the
reader filters task/type/CPU again. `CM_RIGHT_TRACE_READ`/`CAP_TRACE_READ` reuse
INSPECT; read-only duplicated handles cannot configure. Read and configure both
acquire a stable typed capability reference. Last-reference destruction turns
off that session; process EXIT drops trace handles.

Bounded ELF syscalls:
- 22 TRACE_READ(handle, cpu, user_address, unused): user_address points to
  `cm_trace_read_t` (cursor, cumulative lost, one event). Initialize cursor/lost
  to zero or use a known publication cursor. Authorization precedes buffer
  access; writable/readable ELF range checking follows. OK yields an event;
  AGAIN can update overwrite/cursor accounting. DENIED/INVAL expose no payload.
- 23 TRACE_CONFIG(handle, mask, unused, unused): requires WRITE and a mask
  within the object's allowed points. Passing 0 disables the session.

The COW hook reports real block replacement in the existing COW filesystem;
VMO has no page-COW split mechanism and none is fabricated. Fault instrumentation
is the existing anonymous compressed/swapped soft-fault path, not a new hardware
page-fault handler.

`boot/phase8_test.c` checks:
- 16 injected switches + 8 real channel sends/receives = 32 ordered records,
  matching counts, root IDs, parent hops and nondecreasing local TSC.
- Four real CPU callbacks synchronized behind an AP-ready barrier, each writing
  64 records with a distinct TP/payload marker: 256 records, CPU isolation and
  local order. Hosted exercises one CPU explicitly.
- 10,000 disabled calls do not evaluate side effects or advance the head;
  cycles are printed (wall-clock maxima are not asserted under host preemption).
- 520 writes retain 512 and report 8 overwritten records.
- Granted/denied rights, read-only configuration rejection, task/CPU scope and
  mask escalation rejection.
- A real ring3 ELF checks denied read/configure, rejected text buffer, granted
  readback, reported CS CPL=3, prints `trace user rx ok`, and exits 0.

`tests/trace_elf.S` builds to `build/trace.elf`, then the usual embed script makes
`build/trace_elf.inc`. The selftest writes it into live COWFS at `/bin/trace.elf`
and runs the accepted static ELF loader. Phase8 runs before phase4 intentionally
offlines the ramdisk; phase4 crash isolation remains unchanged. Hosted loads the
fixture but explicitly skips privileged ring3 execution. Test fixtures require
the default build mask.

Log keywords:
`trace injected=32 readback=32 verified`, `per-cpu concurrent verified cpus=4
injected=256 readback=256 ordered isolated`, `cap denied verified`,
`cap granted verified`, `user syscall cap granted/denied verified CPL=3`,
`mask disabled verified`, `overwrite allowed verified`, `phase8 selftest: PASS`.

Run make test/kernel/iso and the usual single-node QEMU command. Double-node
regression uses modern QEMU's memory-backend-ram+memdev NUMA form documented
in docs/NUMA.md; output is saved to `/tmp/capmix_numa.log`.
