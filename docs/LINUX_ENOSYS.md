# ENOSYS 驱动的有限兼容（2026-10-17）

> 证据归档：见 [tests/evidence 索引与脱敏规则](../tests/evidence/README.md)。矩阵链接为原轮次留存日志的脱敏副本；本次仅归档，未重跑测试。命令中的 `/tmp` 是新运行的输出位置。

Linux persona 默认仍关闭（`capmix.linux=0`）。本轮不改变原生 ABI、原生
`/bin` 数据或卸载策略；新原生程序仍建议 `.cmx` 或 `/bin/cm/`。
这不是通用 Linux、glibc、任意 BusyBox 或 curl 兼容保证。

## 固定 corpus 与基线

沿用仓库固定集，不临时引入宿主 libc 二进制：

- L0 手写静态 ELF；L1 定制 musl 静态 BusyBox 子集；L2/L3/L5 黄金用例；
- `busybox-sh.elf`：定制 BusyBox **1.36.0**、musl **1.2.5** hush 子集，
  SHA-256 `83576b81f0972e75676b1e595d6e0bd403ce5ef71bbd08fbeefdf283a528e050`；
- 构建配方 `scripts/build-busybox-sh.sh`；源包哈希在
  `third_party/linux-l1/SHA256SUMS`，补丁与配置在 `user/linux/`；
- 固定动态链接/TLS/W^X 黄金集：`tests/fixtures/ldso/SHA256SUMS`。

完整 hush corpus 包括变量/条件、子 shell、外部 exec、管道、6000 字节流、
不存在命令（exit=127）及显式 exit=7。已有 BSS 分配器/窗口等限制继续适用；
参见 [hush](LINUX_BUSYBOX_SH.md)、[L5](LINUX_ABI_L5.md)、[动态链接](LINUX_ABI_LDSO.md)。

改动前，在已验证 ISO（SHA-256
`7a6c3e5c52976b1f8ac06f8ad487d21fe6f0acfe5fc3400f69e73adf8bdffdbb`）
上重新运行 KVM/SMP4/Linux+interp/无网卡全量，得到实际 PASS 日志。
以下是**旧日志可见次数下界**，不能当完整调用频次：

| nr | 名称 | 可见次数 | 来源 |
|---:|---|---:|---|
| 14 | rt_sigprocmask | 43 | hush corpus |
| 13 | rt_sigaction | 14 | hush corpus |
| 9999 | 未定义探针 | 2 | 故意负例 |
| 1073741824 | x32/不支持号 | 2 | 故意负例 |
| 1073741825 | 不支持号 | 1 | 故意负例 |
| 54 | setsockopt | 1 | 故意负例 |
| 55 | getsockopt | 1 | 故意负例 |

旧机制每进程仅打印前八次，socket 部分拒绝仅增加计数；因此不拼接多次启动
来制造排名，也不拿这张表宣称完整 syscall 跟踪。

## 两个号的新增范围

Linux v6.6 x86-64 UAPI：`arch/x86/include/uapi/asm/signal.h`、
`include/uapi/asm-generic/signal-defs.h`。内核 sigset=8 字节；sigaction
为 handler/flags/restorer/mask，32 字节，偏移0/8/16/24，有编译期断言。

- **14 rt_sigprocmask**：单线程掩码元数据，BLOCK/UNBLOCK/SETMASK 和查询；
  SIGKILL/SIGSTOP 位清除；set=NULL 时忽略 how。size!=8 或非法 how → EINVAL；
  坏指针 → EFAULT，失败不更新元数据。
- **13 rt_sigaction**：查询，以及 SIG_DFL/SIG_IGN 的有限登记。
  信号号1..64，9/19不能查询或设置；无效号/size → EINVAL。
  SA_RESTORER 可附带但不执行、不保存为回调；其他 flags、非零 action mask、
  自定义 handler → EOPNOTSUPP。坏指针 → EFAULT。

本篇初版这两个接口只管理元数据，**没有 pending 队列、信号投递、handler 执行、
rt_sigreturn、信号中断 wait 或作业控制**。不能借此声称实现 POSIX 信号。
原有 hush sigaction 失败处理补丁保留：用户 handler 仍会明确失败。
新 spawn 清零；fork 复制掩码和忽略位；成功 exec 保留掩码和忽略位，
失败 exec 不改变状态。没有接受过的自定义 handler，因而无需 reset handler。

## 完整计数与复现

分发器按**最终 -ENOSYS 返回值**统一计数一次；覆盖 socket/file/helper，
不改变其他错误返回。每进程前八行日志仍限流，但汇总不受限流影响。
全局原子计数使用固定 513×8=4104 字节：nr0..511逐号统计，其他号码合并
overflow。overflow 明确表示无法按原号排序，不伪造成 nr512 syscall。
计数是单次启动累计，不是可授权用户全局观测接口；这里只由内核自测输出。
快照逐桶原子读取，不保证所有 CPU 的同一时刻；hush 区间在固定 BSP corpus
运行前后取差值，无其他并行 Linux workload。无动态分配或每次 syscall 日志。

```bash
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
python3 scripts/linux-enosys.py --log /tmp/enosys.log --report /tmp/hush.tsv
python3 scripts/linux-enosys.py --parse-only --log /tmp/enosys.log --scope all
python3 scripts/linux-enosys.py --dual --log /tmp/enosys-numa.log
# 有网卡的全量需先启动已有 TCP/UDP echo 宿主服务：
python3 scripts/tcp-echo.py  # 另一个终端
python3 scripts/udp-echo.py # 再一个终端
python3 scripts/linux-enosys.py --nic e1000 --log /tmp/enosys-net.log
python3 tests/check_tcp_pool.py --wire /tmp/enosys-net.log
# Linux 运行时关 / 编译关，分别构建和运行：
make iso LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
python3 scripts/linux-enosys.py --disabled --scope all --log /tmp/enosys-off.log
make iso LINUX_ABI=0 LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
python3 scripts/linux-enosys.py --disabled --scope all --log /tmp/enosys-compiled-off.log
# 恢复默认产品（Linux关）：
make iso LINUX_ABI=1 LIMINE_CONFIG=boot/limine.conf
```

脚本只启动自己的无数据盘 QEMU，结束后终止自己的进程，保留串口日志；
必须有全量 PASS、汇总尾记录和行数之和一致。`--legacy --parse-only` 仅用于
旧日志，明确输出 lower bounds。`--scope hush` 默认要求固定 hush 零 ENOSYS。
`all` 包含故意负例，非零不等于兼容回退失败。`negative` 专门校验新增 socket
helper nr41和未知号overflow各一次。

新增测试：13/14错误指针/size/掩码/拒绝handler；nr41及高号计数恰一次；
ring3 L2父子各验证fork继承，exec目标验证掩码与忽略位保留；hush七组逐字节
输出/exit/回收及ENOSYS=0。其余未实现号（包括54/55、futex、信号投递相关号）
继续明确拒绝，不为追求计数降低而假装成功。

## 环境结果

最终执行记录见下表；未测试项不标 PASS。

| 环境（2026-10-17） | 实际结果 | 串口/输出文件 |
|---|---|---|
| hosted，Linux开、interp关/开 | 两份全量 PASS (0 failures)；对应 flags 的递归检查通过 | [enosys-hosted-on.log](../tests/evidence/logs/enosys-hosted-on.log)、[enosys-hosted-interp.log](../tests/evidence/logs/enosys-hosted-interp.log) |
| hosted，Linux运行时关/编译关 | 两份全量 PASS (0 failures) | [enosys-hosted-off.log](../tests/evidence/logs/enosys-hosted-off.log)、[enosys-hosted-compiled-off.log](../tests/evidence/logs/enosys-hosted-compiled-off.log) |
| QEMU 8.2.2 KVM SMP4，单/双NUMA，无NIC，Linux+interp开 | 两份全量 PASS (0 failures)，hush total=0 | [enosys-on.log](../tests/evidence/logs/enosys-on.log)、[enosys-dual.log](../tests/evidence/logs/enosys-dual.log) |
| QEMU KVM SMP4，e1000 / virtio-net，Linux+interp开 | 两份全量 PASS (0 failures)，原生TCP multi8、UDP/DHCP/Linux-net递归检查通过 | [enosys-e1000.log](../tests/evidence/logs/enosys-e1000.log)、[enosys-virtio-net-pci.log](../tests/evidence/logs/enosys-virtio-net-pci.log) |
| QEMU Linux运行时关/编译关 | 两份全量 PASS (0 failures)，原生路径保留 | [enosys-off.log](../tests/evidence/logs/enosys-off.log)、[enosys-compiled-off.log](../tests/evidence/logs/enosys-compiled-off.log) |
| VirtualBox 7.2.20r175154，UEFI/SMP4/无NIC，Linux+interp开 | 全量 PASS (0 failures)，hush total=0 | [enosys-vbox.log](../tests/evidence/logs/enosys-vbox.log) |
| QEMU默认产品ISO，Linux关 | 用户shell、原生hello exit=0、poweroff通过 | [enosys-default-product.log](../tests/evidence/logs/enosys-default-product.log) |
| QEMU新安装盘，无ISO，Linux+interp开，两次冷启动 | 产品 shell、动态/TLS、hush、Linux TCP、原生重定向/nc/http-get/httpd/poweroff通过；二次读回持久文件 | [enosys-product-first.log](../tests/evidence/logs/enosys-product-first.log)、[enosys-product-second.log](../tests/evidence/logs/enosys-product-second.log) |

VirtualBox本轮不含网卡测试，不将之前其他任务的网络结果归为本轮PASS。
产品路径不运行全量自测，因此产品行使用命令输出/exit=0、HTTP响应和冷启动
读回断言，不伪称其中含全量selftest PASS。

完成后的准确汇总：hush=0；all=9，其中 nr41=1、54=1、55=1、overflow=6，
均为故意拒绝探针；overflow中的高号不按单号声称精确次数。正常固定 L0/L1/L5、
动态/TLS黄金路径各自的零ENOSYS断言继续保留。

构建：`make kernel` 无新warning/error，`make iso`成功；hosted仍有既存
arch未使用变量/函数warning。最终ISO恢复默认产品配置、Linux默认关。

## 修改文件

- `abi/linux.c`、`abi/linux_socket.c`：统一最终返回计数与快照/输出。
- `abi/linux_signal.c`：13/14有限实现。
- `include/capmix/abi.h`、`linux_uapi.h`、`proc.h`：内部API、固定布局、进程元数据。
- `proc/spawn.c`：Linux fork继承两份元数据；exec原有保留字段路径不变。
- `boot/linux_compat_test.c`、`kernel.c`：机制测试注册、最终汇总。
- `boot/linux_sh_test.c`、`tests/check_linux_sh.py`：固定corpus零ENOSYS及区间汇总。
- `boot/linux_l2_test.c`、`tests/linux_l2.S`、`tests/linux_l2_next.S`：CPL3继承/exec保留断言。
- `scripts/linux-enosys.py`：固定fixture验哈希、启动/采集/排序/断言。
- `Makefile`：新增源码纳入两个构建。
- 本文、`docs/LINUX_BUSYBOX_SH.md`、`docs/README.md`：范围、复现与导航。


## 后续：SIGCHLD 最小子集

新增 nr=127 `rt_sigpending`，以及 nr=13 的 SIGCHLD `SA_NOCLDWAIT` 子集。
只投递阻塞的 SIGCHLD 合并位，默认忽略仍保留 wait4 状态；显式 IGN/NOCLDWAIT 自动回收。
用户 handler 继续 EOPNOTSUPP，rt_sigreturn 继续 ENOSYS 并计数，既有负例与计数断言保留。
策略修改的 EBUSY 限制、线程组边界、hush JOB 仍关闭及测试证据见
[LINUX_SIGCHLD.md](LINUX_SIGCHLD.md)。上文 ENOSYS 排序是该历史轮数据，不改写成新一轮统计。
