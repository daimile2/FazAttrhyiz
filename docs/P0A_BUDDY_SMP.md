# P0a buddy SMP safety

原竞态：未串行化的 intrusive free-list pop/push/coalesce 可重复发页或损坏链表；本轮保留已经存在的全局 IRQ-safe 自旋锁，明确 acquire/release，并补齐无同步计数读取与 dump 快照，再用真实 AP 并发 soak 验证。

审计时 alloc/split/free/coalesce/NUMA region 注册已有同一锁，并未把它们
误写成“此前完全没有锁”。普通链表字段本身不需要逐项原子化：整个
事务被唯一锁串行化，包括 RR cursor、global/per-arena counters 和 page
state。bare metal 在争锁前保存 IF 并 cli，释放锁后仅恢复原 IF；hosted
使用同样的原子锁而不执行 cli。不是可重入锁，不供 NMI 使用。

`cm_mm_free_pages/total_pages` 与 `cm_mm_stats` 的 buddy 字段现在受锁保护。
`cm_buddy_arena_snapshot` 提供单 arena 的稳定快照，dump 在解锁后打印；
commit/slab/pressure 查询和 obs emit 不在 buddy 锁中，避免新的锁顺序反转。
多个子系统的统计不是一个全系统事务快照。初始化仅在 AP 未运行时进行。

独立 AP rendezvous 不修改 phase8 TRACE dispatch，不修改 timer handler 或
调度算法。AP 调度在本测试期间保持原有 parked 状态；worker 自己运行在
各 CPU，普通 IRQ 仍可执行，buddy 临界区自行屏蔽 IRQ。开始时使用所有
参与 worker 的屏障，结束后 join 才统计。ready 超时不允许执行 callback；
join 超时 fail-stop，不能在仍有 worker 操作分配器时继续后续测试。

每 CPU 10,000 轮，每轮同时保留 4 个 order 0..3 块，混用普通 RR 分配和
strict per-node 分配。4 CPU 为 40,000 轮、160,000 次分配及配对释放。
每个基本页以原子 owner CAS 检测重复/重叠，页首偏移 64 和页尾放带 CPU/
轮次的内容标记。释放前验证内容并清 owner，然后 free：不能 free 后才清
owner，否则合法的新分配可能被误判重复。检测到重复时不覆盖/归还冲突
候选，join 后 fail-stop，避免破坏其他 owner 的页。

owner 表和 arena 元数据在独立 BSS，不从被测 buddy 分配；容量针对当前
最多 8×64MiB boot pools，启动前检查。没有扩大 allocator/NUMA 容量。
测试结束验证所有 owner 清零、global free 恢复、各 node 的 total/free 和
每 order block 计数完整恢复。hosted 只单 CPU；不足 4 CPU 明确打印 coverage
skipped，不能把它当作 >=4 CPU 并发证据。

验收：debug ISO (`make iso LIMINE_CONFIG=boot/limine-selftest.conf`) 用
`-smp 4`，检查 `buddy SMP soak: PASS ... duplicate=0 corrupt=0 ... retained=0
free=X/X`，以及 phase3 `PASSED` 和全量 `selftest result: PASS (0 failures)`。
产品 ISO 不跑此测试；验收后 `make iso` 恢复默认产品镜像。
