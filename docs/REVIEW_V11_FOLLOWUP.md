# v11 外部评审核对与数据完整性修复

2026-10-19。针对外部清单逐项核对当前源码；不把能力边界等同于新回归，也不把旧日志当本轮测试。

## 优先修复

1. **cat 数据损坏风险属实。** 内建 cat 的4096B/替换非打印字节规则在重定向时也生效。进一步核对发现外部 cat 同样替换非打印字节，且1MiB限制会提前停止。因此只把内建跳转到外部程序仍不安全。
   - 内建 cat 有输出重定向时原样复制到 EOF，无4096B截断、无额外换行、无文本替换。
   - 内建 cat 直接显示时保留安全的文本预览；超过4096B明确提示并返回 `CM_ERR_NOSPC (-6)`，不再静默成功。恰好4096B且已EOF不误报。
   - 外部 `/bin/cat.faz` 改为原样流式输出到EOF，无先前1MiB总量限制；固定127B缓冲，不预分配文件大小的内存。读/写/关闭失败返回非零，诊断走stderr。文件系统/单次syscall原有上限不变。
   - 复查 ls：内建到256项返回 `CM_ERR_NOSPC`，外部到上限返回exit3；重定向路径会报告失败，不是隐式成功。但部分列表仍可能已写入，重定向不承诺事务撤销。
2. **DEAD errno 映射确实缺失。** `cm_linux_errno(CM_ERR_DEAD)` 现在返回 `-104`（ECONNRESET），与既有 socket 映射一致，机制自测直接断言；其他错误表保持原状。
3. **短命令 nc 缺少引导属实。** 现在 `nc HOST PORT` 先提示 `use run -i /bin/nc.faz HOST PORT`，返回 `-7`，不创建连接。不会偷偷增加控制台权限。显式非交互run仍保留旧权限语义。

**已有磁盘不会自动覆盖旧 shell/cat。** 本轮测试使用新安装盘；只更新ISO或引导内核，并不会修复旧根上已存在的程序。旧盘须备份并显式部署新版 shell/cat，不能通过重新格式化数据盘“升级”。旧 `.elf` 别名也可能保留旧程序，应一并核对。见 [更名兼容策略](FAZATTRHYIZ.md)。

## 其余清单核对

| 项 | 当前判断 | 本轮处理/边界 |
|---|---|---|
| 4 串口interleave | 可见字节仍可交错，但已有 `[SHELL v=1 begin/end/ready seq=...]` 行协议，自动旅程按序列边界断言，不再仅匹配命令回显 | 不宣称已经分离tty/kmsg；保留该体感限制，见 [SHELL](SHELL.md) |
| 5 poll/select/epoll | 尚无完整兼容入口 | 后续独立里程碑，本轮不加伪实现 |
| 6 累计8线程不复用 | 当前不成立：已有退出槽回收及32轮create/join实测 | 见 [LINUX_THREADS](LINUX_THREADS.md) 中线程槽复用矩阵；仍不宣称通用pthread |
| 7 TCP吞吐上限 | 属实：32槽、MSS536、单pending段、RX默认16KiB可选4–32KiB，无window scale/完整CC | 本轮不改TCP/RTO，见 [TCP_POOL](TCP_POOL.md) |
| 8 文件mmap | 只读PRIVATE快照限制仍在 | 写文件映射/MAP_SHARED未支持，保持明确拒绝 |
| 9 hosts-only | 名称查询读hosts，无UDP53解析/缓存框架 | 保持边界，不称完整DNS |
| 10 墙钟 | 现有相对计时不等于可靠RTC墙钟 | 本轮不实现；ACPI PM timer本身是计时源，不自动提供日期 |
| 11 性能分解 | 现有基线数字不等于根因分解 | 延迟与SMP差异仍未关闭，不凭本次功能测试改写旧性能数字 |
| 12 根LICENSE | 已补齐（2026-10-19） | 按作者 daimile（GitHub：daimile2）的明确授权，添加根 [Apache-2.0 LICENSE](../LICENSE)；第三方组件保留原许可证 |
| 13 构建产物/工作树 | `git ls-files`未发现ISO、根capmix.elf或.claude被跟踪；这些本地文件不等于进入Git历史。但capmix-hosted是tracked构建产物 | 保留可复核事实；未清改历史，发布前仍需清理tracked hosted二进制。不能据当前索引证明所有旧历史都干净 |
| 14 发布/tag/旧名 | 本轮不创建release/tag | `capmix.*`参数、capmix.h/cm_*、capmix.elf、磁盘标识继续作为兼容接口保留；更名不再驱动ABI/布局重命名。见 [FAZATTRHYIZ](FAZATTRHYIZ.md) |

## 复现二进制复制回归

```sh
scripts/demo-user-journey.sh prepare /tmp/faz-cat-qemu
python3 tests/check_user_journey.py --backend qemu --lab /tmp/faz-cat-qemu --copy-safety
# 顺序运行，不能与QEMU同时占用硬件虚拟化
scripts/demo-user-journey.sh prepare /tmp/faz-cat-vbox
python3 tests/check_user_journey.py --backend vbox --lab /tmp/faz-cat-vbox --copy-safety
```

脚本通过真实PS/2键盘执行：

```text
cat /bin/shell.faz > /copy-direct.faz
cat < /bin/shell.faz > /copy-stdin.faz
run /bin/cat.faz /bin/shell.faz > /copy-external.faz
```

正常关机后在实验盘补入独立用户态比较探针，再冷启动验证三份副本与原ELF逐字节一致、长度一致、EOF一致；确认源文件超过4096B，包含NUL及高位字节。另导入65536B的全字节夹具，分别经内建/外部cat复制，再一次冷启动比较。探针不依赖屏幕渲染，不把`committed`当作内容正确的证据；还验证屏幕预览超限明确失败，以及nc指引。

本测试不是原子cp实现：`>` 仍先截断输出；同一文件输入输出、掉电、ENOSPC或I/O错误不承诺回滚已有写入。请勿覆盖唯一数据副本。

构建说明：本轮源码构建成功。首次构建日志仍包含既有的 `arch/x86_64/ioapic.c:g_ioapic_phys` 未使用变量警告；未改该架构文件，不把日志描述为完全无警告。

## 本轮环境矩阵与证据

| 日期/环境 | 结果与范围 |
|---|---|
| 2026-10-19 hosted，Linux 编译开 | [全量 PASS (0 failures)](../tests/evidence/logs/cat-integrity-hosted-on.log)，含DEAD errno机制断言 |
| 2026-10-19 hosted，Linux 编译关 | [全量 PASS (0 failures)](../tests/evidence/logs/cat-integrity-hosted-off.log)，Linux按原语义skip |
| 2026-10-19 QEMU/KVM，4 CPU，AHCI/e1000，三次无ISO冷启动 | [产品及复制断言 PASS](../tests/evidence/logs/cat-integrity-qemu-check.log)：[native](../tests/evidence/logs/cat-integrity-qemu-native.log)、[Linux](../tests/evidence/logs/cat-integrity-qemu-linux.log)、[disabled](../tests/evidence/logs/cat-integrity-qemu-disabled.log)、[HTTP客户端](../tests/evidence/logs/cat-integrity-qemu-http-client.log) |
| 2026-10-19 VBox UEFI，4 CPU，PIIX3，AHCI/e1000，三次无ISO冷启动 | [产品及复制断言 PASS](../tests/evidence/logs/cat-integrity-vbox-check.log)：[native](../tests/evidence/logs/cat-integrity-vbox-native.log)、[Linux](../tests/evidence/logs/cat-integrity-vbox-linux.log)、[disabled](../tests/evidence/logs/cat-integrity-vbox-disabled.log)、[HTTP客户端](../tests/evidence/logs/cat-integrity-vbox-http-client.log) |

比较探针与二进制夹具的[SHA-256](../tests/evidence/logs/cat-integrity-fixtures.log)。测试关键字：`copy integrity verified bytes=15128`（实际ELF长度随版本变化）、`copy integrity verified bytes=65536`、`binary cat copy: PASS`、`linux errno: DEAD -> ECONNRESET (-104) verified`。

首次检查器暴露了输出标记格式及`<`按键注入缺口，修正后已用新盘全流程重跑；[首次QEMU检查器失败](../tests/evidence/logs/cat-integrity-first-qemu-check-failed.log)、[首次VBox检查器失败](../tests/evidence/logs/cat-integrity-first-vbox-check-failed.log)保留，不作为PASS证据。新日志可看到实际键入的`cat < ...`，不是把同一条普通路径复制重复算作stdin验证。产品PASS不等于VBox全量debug/性能已验。

最终 QEMU/KVM smp4、Linux/interp开、无网卡[全量 PASS (0 failures)](../tests/evidence/logs/cat-integrity-debug.log)，wire网络按原有语义skip，产品e1000路径另有HTTP实测；[默认产品ISO已恢复构建](../tests/evidence/logs/cat-integrity-product-iso.log)。

后续更新：ioapic未使用变量警告已按编译分支修复；HTTP延迟与hush放置差异已补对照计量，ENOSYS文本预算增至32且poweroff打印按nr直方图。见 [发布前诊断](RELEASE_PERF_DIAGNOSTICS.md)。旧失败/警告原始证据保留。
