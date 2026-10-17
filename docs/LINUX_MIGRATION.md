# Linux persona：受控跨 CPU 迁移

首版只迁移**从未创建线程组、当前无未回收 fork 子进程的单线程 Linux 进程**。
这不是自动负载均衡、线程组迁移或通用 pthread；CapMix 原生 ABI 和默认
`capmix.linux=0` 不变。开启方法沿用 `capmix.linux=1`。

## ABI 与禁止点

Linux x86-64 syscall **203 sched_setaffinity(pid, cpusetsize, mask)**：

- `pid=0` 或自身 TID；不能移动别的进程，其他 PID 返回 `-ESRCH`。
- 首版 `cpusetsize=8`，mask 是用户空间 uint64_t，必须恰好一个在线 CPU 位。
  空/多位/越界 CPU/其他长度返回 `-EINVAL`，非法用户指针返回 `-EFAULT`。
- 对合格进程请求当前 CPU 返回 0；其他 CPU 成功后在**目标 CPU**返回 0。
- 已创建过 CLONE_THREAD 的组（即使其他成员已退出）、fork 子进程、仍有未回收
  fork 子进程的父进程：`-EOPNOTSUPP`。不迁移共享 mm 的单独成员。
- 打开的 Linux socket/pipe 描述符：`-EOPNOTSUPP`。先关闭才可迁移。
  FILE/console 句柄可保留；首版不改变已有 socket/pipe 共享和固定 CPU 纪律。
- 退出/kill 待处理、fd gate 非空或已有迁移请求：`-EAGAIN`。
- 不迁移持锁、阻塞 syscall、futex 等待、ISR、调度器自身；仅从自身 syscall
  返回边界交接。没有对外“任意搬动挂起内核栈”的接口。
- 不支持 CPU 热拔插；目标必须在已启动 CPU 集合中。

204 sched_getaffinity 继续返回可选 CPU 集合（不是单点实际位置）；
309 getcpu 返回实际 CPU/NUMA node。失败请求不修改位置、页表、FS 或资源记账。

## 栈、入口与 FS/TLS

1. dispatch 完整返回、syscall_depth 回到零，资源引用和 gate 已释放。
2. 禁中断，复制完整用户 syscall 寄存器、返回值、用户 RSP、当前 MSR_FS_BASE。
3. 阻止该上下文再次入选，恢复正常内核 GS 相位及调度器 CR3，交回旧 CPU 调度器。
4. 在 runqueue 锁下核对线程槽与线程 ID；只有旧 CPU 已回到自己的调度器栈，
   才重建线程的 kernel landing、清理旧入口快照、更新固定 CPU。
5. 释放旧 CPU 的执行声明，重新发布 RUNNABLE。目标 CPU 使用自己的 GS/TSS，
   同一进程独占的入口栈及既有 full/shadow CR3，通过 `cm_enter_user_regs` 回用户态，
   恢复 FS base 与用户寄存器。KPTI 策略不变。

不携带旧 CPU 的 preempt/block C 续执行栈。进程页表和 backing 不复制，目标 CPU
重新加载 CR3；同一 mm 不在两个 CPU 同时执行，不新增跨 CPU 共享 mm/TLB 协议。
COW、reserve commit、驻留限制沿用 L2/L5；不新增 COW 压缩/换出能力。
TLS 支持范围仍见 LINUX_ABI_TLS.md，不增加 dlopen/pthread TLS 语义。

## 回滚与测试

交接后若目标验证失败或出现 kill，恢复为原 CPU 的新入口现场，返回 `-EAGAIN`；
不重新进入废弃的 syscall 尾帧。kill 随后由既有退出入口处理。
可信 selftest 可注入一次交接后失败；不是用户可修改的 syscall 参数。

`tests/linux_migrate.S` 用仓库链接脚本、Linux marker、无 libc 可重建：
`make linux-migration-verify`。生成 `build/linux-migrate.elf` 后自动内嵌到自测，
写入测试根 `/bin/linux-migrate.elf`；不覆盖产品原生工具。
显式固定汇编 FILE 名称，避免临时对象文件名进入符号表；验证目标会重建第二份并逐字节比较。
固定 SHA-256 见 `tests/linux-migrate.sha256`：
`b1b5a294840855e91c8090b9babe64574101428dc23f1b6aacb9d9ef6d68a21d`。
测试启动单线程探针，在四 CPU 的本地调度器同时运行时完成 CPU0→CPU1→CPU0
四轮往返；检查返回值、getcpu、FS 基址与 TLS 读写、用户栈/寄存器、syscall/exit。
额外保留一份 COW 镜像快照，CPU1 首次写独立页，检查快照不变、真实 COW split
增加；释放快照/进程后检查 free+cache 和 commit 回到基线。
在 CPU1 再做一次真实 fork：父子修改独立，子进程和未回收子进程的父进程
迁移请求均拒绝；wait/reap 后父进程才能迁回。
第二轮注入一次交接失败，要求原 CPU 续跑并重试成功。
超时拒绝强制释放可能仍存活的上下文，显式失败，不伪造 PASS。

日志关键字：

- `linux migrate: ... from=0 to=1 ... off-cpu=1`
- `linux migration user: CPU0/1 roundtrip FS/TLS syscall COW ok`
- `migrations=8 rollback=ok FS/TLS/COW/syscall/exit=ok`
- `linux migration selftest: PASS (0 failures) pages/commit/reap verified`

hosted 只验证参数/禁止点和回收，硬件交接明确 skipped；单 CPU 跳过硬件迁移。
Linux 关闭时整体 skipped。原有 SMP overlap 与 clone/futex 测试继续独立运行。

## 环境矩阵

2026-10-17 实测，QEMU 8.2.2/KVM、512MiB、4 vCPU；VBox 7.2.20、UEFI、512MiB、4 vCPU、AHCI 光驱、无网卡。

| 配置 | 结果与仓库证据 |
| --- | --- |
| hosted Linux/interp 开 | 全量 PASS；[日志](../tests/evidence/logs/migrate-hosted-on.log)，硬件迁移明确 skip |
| hosted 运行关/编译关 | 全量 PASS；[运行关](../tests/evidence/logs/migrate-hosted-off.log)、[编译关](../tests/evidence/logs/migrate-hosted-compiled-off.log) |
| QEMU 单节点 Linux/interp 开、e1000 usernet | 全量 PASS；[日志](../tests/evidence/logs/migrate-on-final.log)、[递归断言](../tests/evidence/logs/migrate-on-assertions.log) |
| QEMU 双节点 Linux/interp 开、无网卡 | 全量 PASS；[日志](../tests/evidence/logs/migrate-dual-final.log)、[断言](../tests/evidence/logs/migrate-dual-assertions.log) |
| QEMU Linux 运行关/编译关 | 全量 PASS；[运行关](../tests/evidence/logs/migrate-off-final.log)、[编译关](../tests/evidence/logs/migrate-compiled-off-final.log) |
| VBox UEFI SMP4 Linux/interp 开 | 全量 PASS；[日志](../tests/evidence/logs/migrate-vbox-final.log)、[断言](../tests/evidence/logs/migrate-vbox-assertions.log) |
| 默认 Linux 关，无 ISO 产品安装盘两次冷启动 | 产品断言 PASS；[首次](../tests/evidence/logs/migrate-product-first.log)、[第二次](../tests/evidence/logs/migrate-product-second.log)、[断言](../tests/evidence/logs/migrate-product-assertions.log)。覆盖原生 hello、重定向/持久化、nc/http-get/httpd、kill/poweroff；产品日志不冒充全量 selftest |

全量包含既有 Linux SMP overlap、clone/futex、hush、静态与动态/TLS/W^X、phase0–9/NUMA；
e1000 单节点另有实际 UDP/TCP 和 Linux socket 路径。未在本轮另测 virtio-net wire。
[内核编译](../tests/evidence/logs/migrate-kernel-final.log)无 warning/error；
hosted 仍有已有的架构静态函数未使用警告。本轮未改其核心或屏蔽警告。

保留失败证据：首轮[超时](../tests/evidence/logs/migrate-qemu.log)及
[诊断](../tests/evidence/logs/migrate-qemu-diag.log)发现历史进程记录仍引用已复用线程槽，
仅比槽指针不足；现在交接同时校验线程 ID 和迁移状态。
另一次双节点在迁移通过后遇到[宿主 KVM entry failed](../tests/evidence/logs/migrate-dual-first-failure.log)，
该批次同时运行 VBox/QEMU。停止 VBox 后单独重跑全量通过；未确认两者因果关系，
不把该失败日志计为 PASS，也未为此修改 VMX/EPT 核心。

复现（e1000 wire 需要另开仓库 TCP/UDP echo peer）：

```sh
make test
make kernel
make linux-migration-verify
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
# 无网卡也可执行全部迁移、SMP、线程机制自测
 timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -nic none -cdrom capmix.iso -serial stdio -display none -no-reboot \
  > /tmp/capmix-migrate.log 2>&1
python3 tests/check_linux_migration.py /tmp/capmix-migrate.log
# Linux 运行关闭
make iso LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
# Linux 编译关闭（同样需要重建 kernel）
make iso LINUX_ABI=0 LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
# 恢复默认产品（Linux 默认关）
make iso LINUX_ABI=1 LIMINE_CONFIG=boot/limine.conf
```

`--hosted`、`--disabled`、`--interp-off`、`--wire` 是宿主断言器的配置说明，必须与
实际启动参数匹配；断言器递归检查既有 SMP/线程/静态/动态/网络测试链。


本轮[构建/产物标识](../tests/evidence/logs/migrate-build-id.log)同时记录 ISO、探针哈希和 kernel PT_LOAD 哈希；
测试 ISO 与最终产品 kernel 的可加载段一致，整份 kernel ELF 的非加载符号表可能含工具链临时名称。
