# 阶段 0：页表 folding、KPTI 与 NUMA 桩

> 历史阶段记录：保留当时实现与验收边界；后续功能可能已扩展。当前状态见
> [内核总览](../KERNEL_OVERVIEW.zh-CN.md)，操作测试见 [VirtualBox手册](../VIRTUALBOX_TESTING.zh-CN.md)。

## 配置与边界

- `CM_PT_MAX_LEVELS=5`：编译时逻辑层级上限，支持 4/5；默认 5。
- `CM_PT_LEVELS=4`：hosted walker 的默认活动级数，支持 4/5。
  freestanding 从已启用的 `CR4.LA57` 读取实际级数，默认启动仍为 4 级。
  未使用的上层在同一 walker 中跳过，无索引、无表分配。
  运行中的长模式不切换 LA57；本阶段没有增加五级启动协议。
- `CM_KPTI=1`：默认开启；`-DCM_KPTI=0` 恢复完整高半空间映射。
  hosted 只验证页表结构，不执行 CR3、IRET、SYSCALL。

配置改变后需要强制重建，例如：

```sh
make -B kernel CFLAGS='-std=c11 -Wall -Wextra -Wshadow -O2 -g -DCM_KPTI=0'
make iso
```

## 页表与入口约定

`cm_paging_pml4()` / `cm_space_create()` 的名称为兼容原调用保留，返回值是
活动层级的根，不保证实际为 PML4。映射、查询与 Global 遍历均按活动级数递归。
walker 遇到大页条目拒绝下降，本阶段仍只建立 4 KiB 叶子。

`cm_space_create()` 创建完整进程页表，共享内核高半空间。
先建立所有用户低半空间根条目，再调用 `cm_space_shadow()` 创建用户根：
低半空间共享已存在的子树，高半空间仅独立映射 `.entry.text` / `.entry.data`。
这些页全部为 supervisor，入口代码只读，入口数据可写。新增低半根条目后，调用者
须重新构造 shadow；已有共享子树内的映射可继续更新。没有添加地址空间销毁 API。

用户根不映射普通内核 text、一般数据/BSS、HHDM 堆、MMIO。
入口窗口包含汇编入口、每 CPU GS 元数据、GDT/TSS/IDT 和入口/IST 栈。
完整页表仍提供系统调用需要的用户缓冲区视图。KPTI 禁用 PGE，且不设置 Global
叶子，以免 CR3 切换后保留完整内核的 TLB 映射；本阶段没有 PCID 优化。

- 首次进入 ring3：`cm_enter_user()` 在入口栈构造 IRET 帧，装载 shadow CR3，IRETQ。
- SYSCALL：SWAPGS，保存 scratch RAX，在接触一般内核栈/数据前装载每 CPU 完整
  进程 CR3；SYSRET 前恢复 shadow CR3。exit 留在完整页表，回到 selftest 收尾。
- 中断：在映射的入口栈保存原 CR3，切到完整内核根，C 处理结束后恢复原 CR3。
  此路径不依赖 GS 状态，覆盖 SWAPGS 附近的入口窗口。
- 异常：切完整内核根后调用原有报告/停机路径。

当前真实 ring3 执行入口是 `cm_usermode_selftest()`。本阶段不是完整多进程
用户线程调度器；后续若添加用户线程，必须在调度时维护其 CPU 的完整/shadow
CR3 元数据，并保证 TSS.RSP0 指向 shadow 可见的入口栈。

涉及人工审查的关键文件：`switch.S`、`syscall.S`、`entry_asm.h`、`usermode.c`、
`paging.c`（CR3/PGE）、`idt.c`（入口描述符/IST）；LAPIC 定时器 C 逻辑保持原行为。

## NUMA 桩

`include/capmix/numa.h` 提供单节点拓扑：所有在线 CPU 与当前连续 buddy arena
都属于 node0，本地距离 10。没有解析 ACPI SRAT，也没有远程节点或迁移算法。

- `cm_numa_node_info()` / `cm_numa_cpu_node()`：节点 CPU mask 和 CPU→node 查询。
- `cm_numa_stats(0, ...)`：在 buddy 锁内一致快照总页数、空闲页数和各 order
  的空闲块数；它们是 node0 的真实 buddy 统计，不是估算。
- `cm_numa_bind_task(task, 0)`：numactl 风格内核 API，记录任务 `memory_node`；
  非零节点返回 NOTSUP，NULL 返回 INVAL。线程内核栈分配使用此策略。
- `cm_page_alloc_node()`：node0 使用现有 buddy，其他节点返回 NULL。

绑定是内存策略桩，不设置 CPU affinity，不迁移已有页，不添加用户态 CLI。

## 验证

每项任务均执行 hosted 测试、内核构建、ISO 构建及以下四核 KVM 串口测试：

```sh
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display none \
  -no-reboot -d cpu_reset 2>&1 | tee /tmp/capmix_run.log
```

QEMU 在 selftest 后 HLT，timeout 的 124 退出码正常；验收依据是串口
`selftest result: PASS (0 failures)`，不能仅以 timeout 退出码判断。
构建须检查 make 本身的状态，避免 grep 无匹配或旧 ISO 掩盖构建失败。

`boot/phase0_test.c` 检查 folding 的表分配数量、稀疏映射、缺页查询、五级不同
PML5 索引、shadow 隔离/权限、node0 绑定及分阶统计和分配/释放守恒。
真实 ring3 测试验证每次 SYSCALL 的原 shadow CR3 和 exit 时完整 CR3，并延长
用户态执行以覆盖 ring3 定时器入口。

另验证 hosted 五级 walker、编译期最高四级和 KPTI 关闭变体；KPTI 关闭变体
也通过相同四核 KVM 路径。实际 LA57 五级硬件启动仍留待后续阶段。
