# L0 全量自测回收断言修复

本次只修复自测资源生命周期，不修改 Linux/CapMix 双轨语义、TCP RTO、buddy 或页表。

## 根因与证据

早期 IPC 自测创建 `backgroundd`，其 `worker_fn` 每次增加 `bg_work`，到201次才退出。
原测试只调度32步就继续执行后续测试，未等待该线程退出、回收4页内核栈。
带网卡时，后续网络测试提供较多调度机会，线程通常早已完成；显式 `-nic none` 时，
该线程延迟到 L0 的两进程 FS/auxv 抢占探针运行期间才完成。

诊断记录：L0 基线 `free+file_cache=13760`，单次加载/销毁与 golden ELF 回收后仍为13760；
两个探针运行时出现 `REAP ... task=backgroundd`，最终为13764。
因此原断言确实发现了全局资源变化，但增加的4页来自其他测试，不是 ELF 少回收。
L2 基线 ISO 和 L3 ISO 都能复现这个未隔离的生命周期。

## 修复

- `boot/kernel.c`：保留 IPC 的32步调度观测，然后有界等待自有后台线程（最多4096步）。
  要求工作计数201且线程槽已 reap；失败仍计入全量 failures，并清理未退出的 fixture。
  在进入后续测试前输出 `sched IPC background joined: work=201 stack/pages reclaimed`。
- `boot/linux_test.c`：**保留严格相等断言**，没有放宽成“页数不少于基线”，没有扣除/忽略4页。
  成功打印 `linux L0 reclaim verified: before=N after=N`，失败打印实际前后值。
- `tests/check_linux_l0.py`：检查 IPC join 日志及 L0 before/after 严格相等；L1–L3 的日志检查链
  复用此检查，避免仅凭最终 PASS 字样验收。

未改 EEVDF 调度算法、线程回收机制、原生 fork 拒绝策略或 Linux COW fork。
定位用的临时调度器逐线程 reap 日志已移除。

## 验收

构建与 hosted：

```sh
make kernel 2>&1 | grep -iE 'warn|error'
make test
make iso LIMINE_CONFIG=boot/limine-linux-selftest.conf
```

关键复现命令（必须显式禁用 QEMU 默认网卡）：

```sh
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -nic none -cdrom capmix.iso -serial stdio -display none -no-reboot -d cpu_reset
```

保存完整日志后：`python3 tests/check_linux_l3.py LOG`；Linux关闭配置加 `--disabled`，
带 echo 后端的网卡配置加 `--wire`。hosted 使用 `--hosted` 检查 L1–L3，并单独核对 join/reclaim 日志。
编译关闭用 `LINUX_ABI=0`；运行时关闭在 selftest cmdline 设置 `capmix.linux=0`。
网卡 echo 回归先运行 `scripts/tcp-echo.py` 和 `scripts/udp-echo.py`。
最终交付 ISO 恢复默认 product 配置，Linux persona 默认仍关闭。

## 最终实测矩阵（2026-10-15）

QEMU 全部使用 KVM、`-cpu host -smp 4 -m 512`，以下每项均有完整最终日志，
结果为 `selftest result: PASS (0 failures)`；日志检查串联 L0–L3。

| 配置 | L0 页基线/回收后 | 结果与日志 |
| --- | --- | --- |
| Linux开，无网卡 | 13764/13764 | PASS；`/tmp/reclaim-on-no-nic.log` |
| Linux开，无网卡重复 | 13764/13764 | PASS；`/tmp/reclaim-on-no-nic-repeat.log` |
| Linux开，e1000 | 13745/13745 | PASS；`/tmp/reclaim-on-e1000.log` |
| Linux开，双NUMA节点，virtio-net+e1000 | 30080/30080 | PASS；`/tmp/reclaim-on-dual-vnet.log` |
| 运行时关，无网卡 | 13764/13764 | PASS；`/tmp/reclaim-runtime-off-no-nic.log` |
| 编译关，无网卡 | 13764/13764 | PASS；`/tmp/reclaim-compiled-off-no-nic.log` |
| 运行时关，e1000 | 13745/13745 | PASS；`/tmp/reclaim-runtime-off-e1000.log` |
| 编译关，双NUMA节点+双网卡 | 30080/30080 | PASS；`/tmp/reclaim-compiled-off-dual.log` |

hosted Linux开/编译关也均全量 PASS (0 failures)，日志分别为
`/tmp/reclaim-hosted-on.log`、`/tmp/reclaim-hosted-off.log`，均验证 join 和页基线相等。
`make kernel` 开/关构建日志没有 warning/error；`make iso` 成功。
本次没有重跑 VirtualBox；修复前失败日志仍保留，不替换历史记录。
