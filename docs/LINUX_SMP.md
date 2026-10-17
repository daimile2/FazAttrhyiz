# Linux persona 多 CPU（2026-10-17）

> 证据归档：见 [tests/evidence 索引与脱敏规则](../tests/evidence/README.md)。矩阵链接为原轮次留存日志的脱敏副本；本次仅归档，未重跑测试。命令中的 `/tmp` 是新运行的输出位置。

默认仍 `capmix.linux=0`。Linux ELF的判定、原生ABI、KPTI策略、buddy锁、
时钟、TCP RTO与COW reserve/commit语义保持不变。本文最初验证多进程；现有 [有界clone/futex线程](LINUX_THREADS.md) 另述，仍非完整pthread。

## 执行位置与 CPU 可见性

Linux不再把非零CPU请求强制改成BSP0。每个独立Linux进程可以在CPU0..N-1
创建/运行；现有用户shell的普通run/run&轮转策略因此也适用于Linux。
可信内核`cm_posix_spawn`可在AP创建Linux ELF；原生AP创建限制保持原样。
未指定cpu的内核spawn默认仍为CPU0；无自动负载均衡；显式单线程迁移见 [LINUX_MIGRATION.md](LINUX_MIGRATION.md)。
通常固定执行位置；合格单线程进程可显式迁移，fork子进程仍跟随父CPU，exec在当前CPU。
同一fork家族不会因此跨CPU执行，避免改变共享socket/fd的既有使用边界。

新增只读Linux接口：

| nr | 接口 | 首版语义 |
|---:|---|---|
| 309 | getcpu(cpu*, node*, cache) | 写当前逻辑CPU和现有NUMA节点；两指针可NULL；cache忽略；坏指针EFAULT |
| 204 | sched_getaffinity(pid,len,mask) | pid=0或当前pid；至少8字节；写8字节可用CPU集合并返回8；其他pid ESRCH；短缓冲EINVAL；坏指针EFAULT |

SMP4返回可用集合`0xf`。这是persona允许选择的CPU集合；调度器当前选择一个
固定执行位置，不因查询而迁移，不保证在集合内各CPU轮转。它不是当前CPU编号；
实际位置由getcpu返回。`sched_setaffinity(203)`新增受控单目标迁移，禁止点和错误码见迁移文档；
也不宣称Linux完整affinity控制。首版最多报告64位mask，内核现有CPU上限不扩大。

## 生命周期同步

创建/原生spawn/Linux fork/镜像reap/完整record回收共用非阻塞生命周期门。
持有者保存并关闭本CPU中断；不持runqueue锁，不等待另一个持有者。
门冲突：spawn返回CM_ERR_AGAIN，Linux fork返回-EAGAIN，reap返回CM_ERR_BUSY。
失败时不分配新PID/页或改变输出pid。调用方可重试；阻塞wait4在BUSY后yield重查，
WNOHANG可返回0。原生child wait也将门冲突转换为非阻塞AGAIN或阻塞重试，
避免把可重试回收冲突变成shell前台命令失败。门不跨wait4的睡眠循环，不新增跨CPU忙等锁依赖。
该门也覆盖原生创建/回收的元数据修改，避免原生与AP同时分配相同proc槽/PID；
不改变原生ELF调度或原生fork=NOTSUP。

Linux父PID在发布前确定；创建门释放后才发布Linux线程，避免AP首次fork撞上自己的创建者。
仍使用published release/acquire排除未初始化线程；退出后须确认线程已离开
入口栈才能销毁页表与record。不强制释放仍在AP执行的现场。
父子权限检查、失败回滚、驻留COW副本预占及销毁uncharge沿用现有路径。
COW页仍不压缩/换出。容量仍受32个proc槽、32fd/进程等既有限额限制。

pipe已使用IRQ安全锁；FILE经既有VFS同步；独立socket有独立对象，fork共享socket
仍在同一固定CPU。UDP临时端口候选序列改为原子分配，最终绑定冲突仍由后端裁决。
不宣称跨CPU共享同一socket的任意并发操作已经产品化。

## 机制验收

`boot/linux_smp_test.c`复用既有AP测试rendezvous，不修改TRACE或AP dispatcher：

1. CPU0..3各自从本CPU创建Linux探针，确认发布后再同时执行。
2. 每个CPL3探针查询getcpu/可用mask、设置和查询自己的FS，并在抢占期反复校验。
   核对exit=0、报告CPU、mask=0xf、抢占发生及原有两次故意ENOSYS。
3. 断言CPU1/2执行TSC区间有重叠；每CPU完成本地reap。
4. CPU1再创建L2黄金ELF，执行真实fork/COW故障/exec/阻塞wait/reap；子进程继承CPU1。
5. AP全部join后核对free pages + file cache及commit与基线一致。

机制测试有8秒期限；若仍有活跃AP现场则拒绝强制回收。hosted不模拟AP硬件，
明确skip；单CPU明确skip；Linux关闭明确skip。CPU查询的指针/错误码机制测试
仍可在hosted运行。

```bash
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
python3 scripts/linux-enosys.py --log /tmp/linux-smp.log
python3 tests/check_linux_smp.py /tmp/linux-smp.log
python3 scripts/linux-enosys.py --dual --log /tmp/linux-smp-numa.log
python3 tests/check_linux_smp.py /tmp/linux-smp-numa.log
```

`check_linux_smp.py`还递归检查原有L0–L5/hush/ldso/TLS/W^X/TCP池断言。
原来的同BSP FS交错测试保留；新增4个探针输出独立核对，未删除旧测试。
SMP探针会额外产生8次故意ENOSYS，所以全量直方图中的负例总数相应增加；
固定hush区间仍须为零。ENOSYS文档中的9次为上一轮基线，不是本轮上限。

## 本轮修改文件

- `proc/spawn.c`：AP Linux创建/fork/wait/回收及生命周期门。
- `proc/control.c`：原生child wait重试回收冲突，保持既有AGAIN语义。
- `abi/linux.c`：204/309；`abi/linux_socket.c`：原子UDP候选序列。
- `boot/linux_smp_test.c`、`boot/linux_compat_test.c`、`boot/kernel.c`：自测及注册。
- `tests/linux_probe.S`、`tests/check_linux_l0.py`、`tests/check_linux_smp.py`：CPL3报告与宿主断言。
- `include/capmix/abi.h`、`Makefile`：声明和构建。
- 本文、L2/L5/ldso/hush文档及README：更新BSP限制说明。

## 实际环境记录

日期：2026-10-17。QEMU 8.2.2/KVM，512MiB，4 vCPU；双节点各256MiB。

| 环境 | 实际结果 | 日志 |
|---|---|---|
| hosted，Linux+interp开 | 全量 PASS (0 failures)，机制通过；AP/CPL3硬件skip | [linux-smp-last-hosted-on.log](../tests/evidence/logs/linux-smp-last-hosted-on.log) |
| hosted，Linux运行时关 / 编译关 | 两份全量 PASS (0 failures) | [linux-smp-last-hosted-off.log](../tests/evidence/logs/linux-smp-last-hosted-off.log)、[linux-smp-last-hosted-compiled-off.log](../tests/evidence/logs/linux-smp-last-hosted-compiled-off.log) |
| QEMU单 / 双NUMA，Linux+interp开，无NIC | 两份全量 PASS (0 failures)；CPU0–3创建/运行/回收，CPU1/2 overlap，CPU1 fork/COW/exec/wait | [linux-smp-last-on.log](../tests/evidence/logs/linux-smp-last-on.log)、[linux-smp-last-dual.log](../tests/evidence/logs/linux-smp-last-dual.log) |
| QEMU e1000 / virtio-net，Linux+interp开 | 两份全量 PASS (0 failures)；SMP及TCP multi8/UDP/DHCP/Linux-net递归断言通过 | [linux-smp-last-e1000.log](../tests/evidence/logs/linux-smp-last-e1000.log)、[linux-smp-last-virtio-net-pci.log](../tests/evidence/logs/linux-smp-last-virtio-net-pci.log) |
| QEMU Linux运行时关 / 编译关 | 两份全量 PASS (0 failures)；原生保留 | [linux-smp-last-off.log](../tests/evidence/logs/linux-smp-last-off.log)、[linux-smp-last-compiled-off.log](../tests/evidence/logs/linux-smp-last-compiled-off.log) |
| VirtualBox 7.2.20r175154，UEFI/4CPU/无NIC，Linux+interp开 | 全量 PASS (0 failures)，4CPU/AP生命周期及overlap递归断言通过 | [linux-smp-last-vbox.log](../tests/evidence/logs/linux-smp-last-vbox.log) |

上述最终版本包含原生child-wait回收冲突的重试桥接。VBox本轮未测试网络，
不将QEMU网络结果计入VBox。构建kernel无新warning/error，ISO成功；hosted仍有
既存arch未使用变量/函数warning。最终ISO恢复默认产品、Linux默认关。

产品路径最终验证（不运行全量selftest，不混称全量PASS）：

| 环境 | 实际断言 | 日志 |
|---|---|---|
| QEMU默认产品ISO，Linux关 | 用户shell、原生hello exit=0、ACPI poweroff | [linux-smp-last-default-product.log](../tests/evidence/logs/linux-smp-last-default-product.log) |
| QEMU新安装virtio盘/e1000，无ISO，Linux+interp开 | TLS在CPU1/2、动态黄金在CPU3、Linux TCP在CPU0/1、hush在CPU2；各命令exit=0；原生重定向/nc/http-get/httpd响应及poweroff通过 | [linux-smp-last-product-first.log](../tests/evidence/logs/linux-smp-last-product-first.log) |
| 同一盘第二次无ISO冷启动 | 持久文件读回、动态/TLS/hush exit=0、poweroff | [linux-smp-last-product-second.log](../tests/evidence/logs/linux-smp-last-product-second.log) |

修改清单还包括 `docs/KERNEL_OVERVIEW.zh-CN.md`、`docs/LINUX_ABI_L0.md` 的
导航/现状说明；不改原生user shell、KPTI、buddy、时钟、TCP RTO或IRQ核心。

线程续作：已有 [有界clone/futex](LINUX_THREADS.md)。本文单线程/不支持线程的说明为此前范围；
新子集仍不等于通用pthread，线程组内fork/exec/映射变更仍拒绝；fd仅放宽[FILE/console dup/close白名单](LINUX_THREADS.md#受控fd白名单与仍冻结范围)，未创建线程的原路径保留。
