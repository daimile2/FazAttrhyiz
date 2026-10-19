# CapMix 验收日志归档

归档日期：2026-10-17。最初归档轮次仅整理已有证据，没有重跑虚拟机。后续TCP_POOL/e1000修复及线程fd白名单已新增实际运行证据，见下节与逐文件`evidence_round`。
各测试的日期、版本、硬件配置和验收范围以对应文档原环境矩阵为准，归档日期不是重测日期。

## 快速入口

| 项目 | QEMU | VirtualBox | 对应范围 |
| --- | --- | --- | --- |
| shell 重定向/冷启动 | [首次](logs/redir-qemu-final1.log)、[冷启动](logs/redir-qemu-final2.log) | [首次](logs/redir-vbox-final1.log)、[冷启动](logs/redir-vbox-final2.log) | 产品路径；VBox本轮未验网络/全量debug |
| 静态 BusyBox hush | [全量开启](logs/sh-on-e1000-matrix.log)、[冷启动](logs/sh-product-second.log) | [首次](logs/sh-vbox-first.log)、[冷启动](logs/sh-vbox-second.log) | 定制musl静态子集；VBox产品路径 |
| 真 ld.so 最小路径 | [全量开启](logs/ldso-on-e1000-matrix.log)、[冷启动](logs/ldso-product-second.log) | [首次](logs/ldso-vbox-first.log)、[冷启动](logs/ldso-vbox-second.log) | 钉死黄金重定位；VBox产品路径 |
| 最小 TLS | [全量开启](logs/tls-on-e1000-matrix.log)、[冷启动](logs/tls-final-product-second.log) | [首次](logs/tls-vbox-first.log)、[冷启动](logs/tls-vbox-second.log) | 有界TLS黄金探针；VBox产品路径 |
| W^X/RELRO | [全量开启](logs/wx-on-e1000-matrix.log) | [首次](logs/wx-vbox-first.log)、[冷启动](logs/wx-vbox-second.log) | VBox产品黄金，不代替全量安全机制测试 |
| 后续线程/futex回归 | [单节点](logs/futex-final-on.log)、[双节点](logs/futex-final-dual.log) | [全量debug，无NIC](logs/futex-final-vbox.log) | 该轮VBox有全量PASS，仍不能推出网络PASS |

## 日志内容与脱敏

`logs/`保留原日志的完整文本顺序，包括失败、拒绝、skip、退出码和关机记录，方便检查命令回显与真正输出的区别。
移除ANSI控制序列、统一换行；宿主home路径、临时文件路径、UUID、MAC、指定测试VM名称以及高半区内核地址替换为占位符。
保留固定guest路径、测试IPv4地址（如10.0.2.2/10.0.2.15、规则夹具10.0.0.1）、端口、用户地址、错误码、哈希与计数。
这些是测试参数，不是公开可达主机或凭据。未归档虚拟磁盘、VM配置、ISO及个人文件。

`manifest.json`记录源日志文件名、原始字节SHA-256、脱敏副本SHA-256、源长度、关联文档及是否出现全量PASS标志。
源哈希用于标识原始版本；副本哈希用于检查仓库归档完整性。脱敏文本不与原始哈希相同。
PASS标志字段只说明字符串存在，不能代替整段日志/文档矩阵范围判断；产品测试没有全量PASS标志也是正常的。
历史L0产品#PF、L2/L3旧reclaim失败基线一并保留，它们不是成功验收证据。

## 复核

在仓库根目录运行：

```bash
python3 tests/evidence/verify.py
python3 tests/check_shell_redirection.py qemu tests/evidence/logs/redir-qemu-final2.log
python3 tests/check_shell_redirection.py vbox tests/evidence/logs/redir-vbox-final2.log
```

第一条只检查哈希、脱敏和链接，不运行guest。其余脚本检查留存的shell冷启动内容。
ABI检查器可能随实现演进而增加断言；旧轮次日志应按其原文档范围阅读，不能拿今天的新断言冒充当时测试结果。
文档复现命令中的`/tmp/*.log`仍是新运行输出位置，不是未归档的矩阵链接。

## 按文档查全部归档

### [LINUX_ABI_L0.md](../../docs/LINUX_ABI_L0.md)

[linux-l0-hosted-final.log](logs/linux-l0-hosted-final.log)、[linux-l0-hosted-off-final.log](logs/linux-l0-hosted-off-final.log)、[linux-l0-product-off-final.log](logs/linux-l0-product-off-final.log)、[linux-l0-product-off.log](logs/linux-l0-product-off.log)、[linux-l0-product-on-final.log](logs/linux-l0-product-on-final.log)、[linux-l0-product-on.log](logs/linux-l0-product-on.log)、[linux-l0-qemu-compiled-off-final.log](logs/linux-l0-qemu-compiled-off-final.log)、[linux-l0-qemu-on-final.log](logs/linux-l0-qemu-on-final.log)、[linux-l0-qemu-runtime-off-final.log](logs/linux-l0-qemu-runtime-off-final.log)、[linux-l0-vbox-httpd-client.log](logs/linux-l0-vbox-httpd-client.log)、[linux-l0-vbox-product.log](logs/linux-l0-vbox-product.log)

### [LINUX_ABI_L1.md](../../docs/LINUX_ABI_L1.md)

[linux-l1-hosted-final.log](logs/linux-l1-hosted-final.log)、[linux-l1-hosted-off-final.log](logs/linux-l1-hosted-off-final.log)、[linux-l1-httpd-client-off.log](logs/linux-l1-httpd-client-off.log)、[linux-l1-httpd-client.log](logs/linux-l1-httpd-client.log)、[linux-l1-product-off.log](logs/linux-l1-product-off.log)、[linux-l1-product-on.log](logs/linux-l1-product-on.log)、[linux-l1-qemu-compiled-off-final.log](logs/linux-l1-qemu-compiled-off-final.log)、[linux-l1-qemu-on-final.log](logs/linux-l1-qemu-on-final.log)、[linux-l1-qemu-runtime-off-final.log](logs/linux-l1-qemu-runtime-off-final.log)、[linux-l1-qemu-single-on-final.log](logs/linux-l1-qemu-single-on-final.log)、[linux-l1-repro-verify.log](logs/linux-l1-repro-verify.log)、[linux-l1-vbox-coldboot.log](logs/linux-l1-vbox-coldboot.log)、[linux-l1-vbox-firstboot.log](logs/linux-l1-vbox-firstboot.log)、[linux-l1-verify.log](logs/linux-l1-verify.log)

### [LINUX_ABI_L2.md](../../docs/LINUX_ABI_L2.md)

[linux-l2-fixture-verify.log](logs/linux-l2-fixture-verify.log)、[linux-l2-hosted-final.log](logs/linux-l2-hosted-final.log)、[linux-l2-hosted-off-final.log](logs/linux-l2-hosted-off-final.log)、[linux-l2-httpd-client-off.log](logs/linux-l2-httpd-client-off.log)、[linux-l2-httpd-client.log](logs/linux-l2-httpd-client.log)、[linux-l2-product-cold.log](logs/linux-l2-product-cold.log)、[linux-l2-product-off.log](logs/linux-l2-product-off.log)、[linux-l2-product-on.log](logs/linux-l2-product-on.log)、[linux-l2-qemu-compiled-off-final.log](logs/linux-l2-qemu-compiled-off-final.log)、[linux-l2-qemu-on-final.log](logs/linux-l2-qemu-on-final.log)、[linux-l2-qemu-runtime-off-final.log](logs/linux-l2-qemu-runtime-off-final.log)、[linux-l2-qemu-single-on-final.log](logs/linux-l2-qemu-single-on-final.log)、[linux-l2-vbox-product.log](logs/linux-l2-vbox-product.log)

### [LINUX_ABI_L3.md](../../docs/LINUX_ABI_L3.md)

[linux-l3-hosted-off-final.log](logs/linux-l3-hosted-off-final.log)、[linux-l3-hosted-release.log](logs/linux-l3-hosted-release.log)、[linux-l3-native-httpd-client.log](logs/linux-l3-native-httpd-client.log)、[linux-l3-native-off-httpd.log](logs/linux-l3-native-off-httpd.log)、[linux-l3-product-final-client.log](logs/linux-l3-product-final-client.log)、[linux-l3-product-final.log](logs/linux-l3-product-final.log)、[linux-l3-product-off.log](logs/linux-l3-product-off.log)、[linux-l3-product.log](logs/linux-l3-product.log)、[linux-l3-qemu-compiled-off-final.log](logs/linux-l3-qemu-compiled-off-final.log)、[linux-l3-qemu-dual-l2-baseline-no-nic-final.log](logs/linux-l3-qemu-dual-l2-baseline-no-nic-final.log)、[linux-l3-qemu-dual-no-nic-final.log](logs/linux-l3-qemu-dual-no-nic-final.log)、[linux-l3-qemu-dual-on-final.log](logs/linux-l3-qemu-dual-on-final.log)、[linux-l3-qemu-dual-single-final.log](logs/linux-l3-qemu-dual-single-final.log)、[linux-l3-qemu-runtime-off-final.log](logs/linux-l3-qemu-runtime-off-final.log)、[linux-l3-server-after-kill.log](logs/linux-l3-server-after-kill.log)、[linux-l3-vbox-client.log](logs/linux-l3-vbox-client.log)、[linux-l3-vbox-final.log](logs/linux-l3-vbox-final.log)

### [LINUX_ABI_L4.md](../../docs/LINUX_ABI_L4.md)

[l4-compiled-off-final.log](logs/l4-compiled-off-final.log)、[l4-hosted-interp-off.log](logs/l4-hosted-interp-off.log)、[l4-hosted-linux-off.log](logs/l4-hosted-linux-off.log)、[l4-hosted-off.log](logs/l4-hosted-off.log)、[l4-hosted-on-final.log](logs/l4-hosted-on-final.log)、[l4-interp-off-final.log](logs/l4-interp-off-final.log)、[l4-linux-off-final.log](logs/l4-linux-off-final.log)、[l4-native-httpd-client.log](logs/l4-native-httpd-client.log)、[l4-on-dual-vnet-final.log](logs/l4-on-dual-vnet-final.log)、[l4-on-e1000-final.log](logs/l4-on-e1000-final.log)、[l4-on-no-nic-final.log](logs/l4-on-no-nic-final.log)、[l4-product.log](logs/l4-product.log)、[l4-vbox-product.log](logs/l4-vbox-product.log)

### [LINUX_ABI_L5.md](../../docs/LINUX_ABI_L5.md)

[l5-compiled-off-matrix.log](logs/l5-compiled-off-matrix.log)、[l5-default-product.log](logs/l5-default-product.log)、[l5-hosted-final.log](logs/l5-hosted-final.log)、[l5-hosted-off.log](logs/l5-hosted-off.log)、[l5-httpd-client.log](logs/l5-httpd-client.log)、[l5-linux-off-matrix.log](logs/l5-linux-off-matrix.log)、[l5-on-dual-vnet-matrix.log](logs/l5-on-dual-vnet-matrix.log)、[l5-on-e1000-matrix.log](logs/l5-on-e1000-matrix.log)、[l5-on-no-nic-matrix.log](logs/l5-on-no-nic-matrix.log)、[l5-product-first.log](logs/l5-product-first.log)、[l5-product-installboot.log](logs/l5-product-installboot.log)、[l5-vbox-first.log](logs/l5-vbox-first.log)、[l5-vbox-second.log](logs/l5-vbox-second.log)

### [LINUX_ABI_LDSO.md](../../docs/LINUX_ABI_LDSO.md)

[ldso-build1.log](logs/ldso-build1.log)、[ldso-build2.log](logs/ldso-build2.log)、[ldso-compiled-off-matrix.log](logs/ldso-compiled-off-matrix.log)、[ldso-default-product.log](logs/ldso-default-product.log)、[ldso-hosted-off.log](logs/ldso-hosted-off.log)、[ldso-hosted-on.log](logs/ldso-hosted-on.log)、[ldso-hosted.log](logs/ldso-hosted.log)、[ldso-httpd-client.log](logs/ldso-httpd-client.log)、[ldso-interp-off-matrix.log](logs/ldso-interp-off-matrix.log)、[ldso-linux-off-matrix.log](logs/ldso-linux-off-matrix.log)、[ldso-on-dual-vnet-matrix.log](logs/ldso-on-dual-vnet-matrix.log)、[ldso-on-e1000-matrix.log](logs/ldso-on-e1000-matrix.log)、[ldso-on-no-nic-matrix.log](logs/ldso-on-no-nic-matrix.log)、[ldso-product-installboot.log](logs/ldso-product-installboot.log)、[ldso-product-interp-off.log](logs/ldso-product-interp-off.log)、[ldso-product-second.log](logs/ldso-product-second.log)、[ldso-vbox-first.log](logs/ldso-vbox-first.log)、[ldso-vbox-second.log](logs/ldso-vbox-second.log)

### [LINUX_ABI_TLS.md](../../docs/LINUX_ABI_TLS.md)

[tls-compiled-off-matrix.log](logs/tls-compiled-off-matrix.log)、[tls-default-product.log](logs/tls-default-product.log)、[tls-final-httpd-client.log](logs/tls-final-httpd-client.log)、[tls-final-product-first.log](logs/tls-final-product-first.log)、[tls-final-product-second.log](logs/tls-final-product-second.log)、[tls-fixture-build.log](logs/tls-fixture-build.log)、[tls-hosted-disabled.log](logs/tls-hosted-disabled.log)、[tls-hosted-interp-off.log](logs/tls-hosted-interp-off.log)、[tls-hosted-on.log](logs/tls-hosted-on.log)、[tls-interp-off-matrix.log](logs/tls-interp-off-matrix.log)、[tls-linux-off-matrix.log](logs/tls-linux-off-matrix.log)、[tls-on-dual-vnet-matrix.log](logs/tls-on-dual-vnet-matrix.log)、[tls-on-e1000-matrix.log](logs/tls-on-e1000-matrix.log)、[tls-on-no-nic-matrix.log](logs/tls-on-no-nic-matrix.log)、[tls-reproduce.log](logs/tls-reproduce.log)、[tls-vbox-first.log](logs/tls-vbox-first.log)、[tls-vbox-second.log](logs/tls-vbox-second.log)

### [LINUX_ABI_WX_RELRO.md](../../docs/LINUX_ABI_WX_RELRO.md)

[wx-compiled-off-matrix.log](logs/wx-compiled-off-matrix.log)、[wx-default-product.log](logs/wx-default-product.log)、[wx-final-httpd-client.log](logs/wx-final-httpd-client.log)、[wx-final-product-first.log](logs/wx-final-product-first.log)、[wx-final-product-second.log](logs/wx-final-product-second.log)、[wx-fixture-build.log](logs/wx-fixture-build.log)、[wx-interp-off-matrix.log](logs/wx-interp-off-matrix.log)、[wx-linux-off-matrix.log](logs/wx-linux-off-matrix.log)、[wx-on-dual-vnet-matrix.log](logs/wx-on-dual-vnet-matrix.log)、[wx-on-e1000-matrix.log](logs/wx-on-e1000-matrix.log)、[wx-on-no-nic-matrix.log](logs/wx-on-no-nic-matrix.log)、[wx-reproduce.log](logs/wx-reproduce.log)、[wx-vbox-first.log](logs/wx-vbox-first.log)、[wx-vbox-second.log](logs/wx-vbox-second.log)

### [LINUX_BUSYBOX_SH.md](../../docs/LINUX_BUSYBOX_SH.md)

[sh-compiled-off-matrix.log](logs/sh-compiled-off-matrix.log)、[sh-default-product.log](logs/sh-default-product.log)、[sh-hosted-final.log](logs/sh-hosted-final.log)、[sh-hosted-off.log](logs/sh-hosted-off.log)、[sh-httpd-client.log](logs/sh-httpd-client.log)、[sh-linux-off-matrix.log](logs/sh-linux-off-matrix.log)、[sh-on-dual-vnet-matrix.log](logs/sh-on-dual-vnet-matrix.log)、[sh-on-e1000-matrix.log](logs/sh-on-e1000-matrix.log)、[sh-on-no-nic-matrix.log](logs/sh-on-no-nic-matrix.log)、[sh-product-installboot.log](logs/sh-product-installboot.log)、[sh-product-off.log](logs/sh-product-off.log)、[sh-product-second.log](logs/sh-product-second.log)、[sh-repro.log](logs/sh-repro.log)、[sh-repro2.log](logs/sh-repro2.log)、[sh-vbox-first.log](logs/sh-vbox-first.log)、[sh-vbox-second.log](logs/sh-vbox-second.log)

### [LINUX_ENOSYS.md](../../docs/LINUX_ENOSYS.md)

[enosys-compiled-off.log](logs/enosys-compiled-off.log)、[enosys-default-product.log](logs/enosys-default-product.log)、[enosys-dual.log](logs/enosys-dual.log)、[enosys-e1000.log](logs/enosys-e1000.log)、[enosys-hosted-compiled-off.log](logs/enosys-hosted-compiled-off.log)、[enosys-hosted-interp.log](logs/enosys-hosted-interp.log)、[enosys-hosted-off.log](logs/enosys-hosted-off.log)、[enosys-hosted-on.log](logs/enosys-hosted-on.log)、[enosys-off.log](logs/enosys-off.log)、[enosys-on.log](logs/enosys-on.log)、[enosys-product-first.log](logs/enosys-product-first.log)、[enosys-product-second.log](logs/enosys-product-second.log)、[enosys-vbox.log](logs/enosys-vbox.log)、[enosys-virtio-net-pci.log](logs/enosys-virtio-net-pci.log)

### [LINUX_SMP.md](../../docs/LINUX_SMP.md)

[linux-smp-last-compiled-off.log](logs/linux-smp-last-compiled-off.log)、[linux-smp-last-default-product.log](logs/linux-smp-last-default-product.log)、[linux-smp-last-dual.log](logs/linux-smp-last-dual.log)、[linux-smp-last-e1000.log](logs/linux-smp-last-e1000.log)、[linux-smp-last-hosted-compiled-off.log](logs/linux-smp-last-hosted-compiled-off.log)、[linux-smp-last-hosted-off.log](logs/linux-smp-last-hosted-off.log)、[linux-smp-last-hosted-on.log](logs/linux-smp-last-hosted-on.log)、[linux-smp-last-off.log](logs/linux-smp-last-off.log)、[linux-smp-last-on.log](logs/linux-smp-last-on.log)、[linux-smp-last-product-first.log](logs/linux-smp-last-product-first.log)、[linux-smp-last-product-second.log](logs/linux-smp-last-product-second.log)、[linux-smp-last-vbox.log](logs/linux-smp-last-vbox.log)、[linux-smp-last-virtio-net-pci.log](logs/linux-smp-last-virtio-net-pci.log)

### [LINUX_THREADS.md](../../docs/LINUX_THREADS.md)

[futex-final-compiled-off.log](logs/futex-final-compiled-off.log)、[futex-final-default-product.log](logs/futex-final-default-product.log)、[futex-final-dual.log](logs/futex-final-dual.log)、[futex-final-e1000.log](logs/futex-final-e1000.log)、[futex-final-hosted-compiled-off.log](logs/futex-final-hosted-compiled-off.log)、[futex-final-hosted-off.log](logs/futex-final-hosted-off.log)、[futex-final-hosted-on.log](logs/futex-final-hosted-on.log)、[futex-final-off.log](logs/futex-final-off.log)、[futex-final-on.log](logs/futex-final-on.log)、[futex-final-product-first.log](logs/futex-final-product-first.log)、[futex-final-product-second.log](logs/futex-final-product-second.log)、[futex-final-static.log](logs/futex-final-static.log)、[futex-final-vbox.log](logs/futex-final-vbox.log)、[futex-final-virtio-net-pci.log](logs/futex-final-virtio-net-pci.log)、[futex-fixture-verify-final.log](logs/futex-fixture-verify-final.log)

### [SHELL.md](../../docs/SHELL.md)

[redir-hosted-final.log](logs/redir-hosted-final.log)、[redir-qemu-debug-final.log](logs/redir-qemu-debug-final.log)、[redir-qemu-final1.log](logs/redir-qemu-final1.log)、[redir-qemu-final2.log](logs/redir-qemu-final2.log)、[redir-vbox-final1.log](logs/redir-vbox-final1.log)、[redir-vbox-final2.log](logs/redir-vbox-final2.log)


## TCP_POOL / VBox e1000 debug修复（2026-10-17）

[四项失败对账、关闭说明与完整矩阵](../../docs/TCP_POOL.md#vbox-e1000-debug原4项失败对账与关闭)。
旧失败与新结果分开保留；拔线的全量0 failures不代表wire PASS。
本轮新增checker要求实际载荷/IRQ/multi8，拔线checker则要求明确skip与无NIC回收路径。

| 场景 | 证据 |
| --- | --- |
| 原FAIL(4)，独立VM复现 | [历史](logs/tcp32-vbox-debug.log)、[复现](logs/e1000fix-vbox-baseline.log) |
| VBox LinkUpDelay=5000ms | [设备配置摘录](logs/e1000fix-vbox-device.log) |
| VBox链路up，Linux关，全量与wire PASS | [日志](logs/e1000fix-vbox-off-final.log)、[断言](logs/e1000fix-check-vbox-off-final.log) |
| VBox拔线，wire明确SKIP，机制PASS | [日志](logs/e1000fix-vbox-down-final.log)、[断言](logs/e1000fix-check-vbox-down-final.log) |
| QEMU e1000 / 双NUMA virtio+e1000 | [单节点](logs/e1000fix-qemu-e1000-final.log)、[双节点](logs/e1000fix-qemu-dual-final.log) |
| 无ISO产品网络/重定向/kill/关机与冷启动 | [首次](logs/e1000fix-final-product-first.log)、[冷启动](logs/e1000fix-final-product-second.log)、[断言](logs/e1000fix-final-product-assertions.log) |

新增及原TCP_POOL归档文件：

[e1000fix-build-id.log](logs/e1000fix-build-id.log)、[e1000fix-check-baseline-rejected.log](logs/e1000fix-check-baseline-rejected.log)、[e1000fix-check-hosted-off-final.log](logs/e1000fix-check-hosted-off-final.log)、[e1000fix-check-qemu-default-debug.log](logs/e1000fix-check-qemu-default-debug.log)、[e1000fix-check-qemu-dual-final.log](logs/e1000fix-check-qemu-dual-final.log)、[e1000fix-check-qemu-e1000-final.log](logs/e1000fix-check-qemu-e1000-final.log)、[e1000fix-check-vbox-down-final.log](logs/e1000fix-check-vbox-down-final.log)、[e1000fix-check-vbox-off-final.log](logs/e1000fix-check-vbox-off-final.log)、[e1000fix-default-product-assertions.log](logs/e1000fix-default-product-assertions.log)、[e1000fix-default-product.log](logs/e1000fix-default-product.log)、[e1000fix-final-install.log](logs/e1000fix-final-install.log)、[e1000fix-final-product-assertions.log](logs/e1000fix-final-product-assertions.log)、[e1000fix-final-product-first.log](logs/e1000fix-final-product-first.log)、[e1000fix-final-product-httpd-client.log](logs/e1000fix-final-product-httpd-client.log)、[e1000fix-final-product-second.log](logs/e1000fix-final-product-second.log)、[e1000fix-hosted-off-final.log](logs/e1000fix-hosted-off-final.log)、[e1000fix-iso-final.log](logs/e1000fix-iso-final.log)、[e1000fix-iso-product-final.log](logs/e1000fix-iso-product-final.log)、[e1000fix-kernel-final-diagnostics.log](logs/e1000fix-kernel-final-diagnostics.log)、[e1000fix-kernel-final.log](logs/e1000fix-kernel-final.log)、[e1000fix-primary.log](logs/e1000fix-primary.log)、[e1000fix-qemu-default-debug.log](logs/e1000fix-qemu-default-debug.log)、[e1000fix-qemu-dual-final.log](logs/e1000fix-qemu-dual-final.log)、[e1000fix-qemu-e1000-final.log](logs/e1000fix-qemu-e1000-final.log)、[e1000fix-vbox-baseline.log](logs/e1000fix-vbox-baseline.log)、[e1000fix-vbox-configuration.log](logs/e1000fix-vbox-configuration.log)、[e1000fix-vbox-device.log](logs/e1000fix-vbox-device.log)、[e1000fix-vbox-down-final.log](logs/e1000fix-vbox-down-final.log)、[e1000fix-vbox-off-final.log](logs/e1000fix-vbox-off-final.log)、[tcp32-compiled-off-matrix.log](logs/tcp32-compiled-off-matrix.log)、[tcp32-default-product.log](logs/tcp32-default-product.log)、[tcp32-final-httpd-client.log](logs/tcp32-final-httpd-client.log)、[tcp32-final-product-first.log](logs/tcp32-final-product-first.log)、[tcp32-final-product-second.log](logs/tcp32-final-product-second.log)、[tcp32-final-vbox-debug-nonic.log](logs/tcp32-final-vbox-debug-nonic.log)、[tcp32-final-vbox-first.log](logs/tcp32-final-vbox-first.log)、[tcp32-final-vbox-second.log](logs/tcp32-final-vbox-second.log)、[tcp32-hosted-off.log](logs/tcp32-hosted-off.log)、[tcp32-hosted-on.log](logs/tcp32-hosted-on.log)、[tcp32-hosted-rx32768.log](logs/tcp32-hosted-rx32768.log)、[tcp32-hosted-rx4096.log](logs/tcp32-hosted-rx4096.log)、[tcp32-hosted-static.log](logs/tcp32-hosted-static.log)、[tcp32-interp-off-matrix.log](logs/tcp32-interp-off-matrix.log)、[tcp32-invalid-rx32769.log](logs/tcp32-invalid-rx32769.log)、[tcp32-invalid-rx4095.log](logs/tcp32-invalid-rx4095.log)、[tcp32-linux-off-matrix.log](logs/tcp32-linux-off-matrix.log)、[tcp32-on-dual-vnet-matrix.log](logs/tcp32-on-dual-vnet-matrix.log)、[tcp32-on-e1000-matrix.log](logs/tcp32-on-e1000-matrix.log)、[tcp32-on-no-nic-matrix.log](logs/tcp32-on-no-nic-matrix.log)、[tcp32-rx32768-matrix.log](logs/tcp32-rx32768-matrix.log)、[tcp32-rx4096-matrix.log](logs/tcp32-rx4096-matrix.log)、[tcp32-vbox-debug.log](logs/tcp32-vbox-debug.log)

## 线程组fd白名单（2026-10-17）

[支持/拒绝表、黄金SHA及环境矩阵](../../docs/LINUX_THREADS.md#fd白名单续作验收2026-10-17)。
默认Linux仍关，不是通用pthread；fd0–2、pipe/socket修改以及fork/exec/映射变更仍拒绝。
本轮实际启动QEMU和VBox；首轮VBox失败、阶段诊断和缺少宿主网络测试端的失败均保留，未改写为PASS。
负syscall返回rax在脱敏副本转成十进制（例如-11）；源日志哈希仍对应原字节。

| 场景 | 证据 |
| --- | --- |
| hosted 机制/竞争/回收 | [Linux开](logs/threadfd-hosted-on-final.log)、[运行关](logs/threadfd-hosted-off-final.log)、[编译关](logs/threadfd-hosted-compiled-off.log) |
| QEMU 真线程/hush/静态/动态/回收 | [单节点](logs/threadfd-on-final.log)、[双节点](logs/threadfd-dual-final.log)、[interp关](logs/threadfd-static-final.log) |
| QEMU 原生关闭矩阵 | [运行关](logs/threadfd-off-final.log)、[编译关](logs/threadfd-compiled-off-final.log) |
| VBox UEFI/SMP4/no-NIC，修复后两次全量PASS | [通过](logs/threadfd-vbox-fixed.log)、[复测](logs/threadfd-vbox-repeat.log) |
| VBox 首轮失败，fork生命周期门拒绝次序问题 | [原失败](logs/threadfd-vbox-first-failure.log)、[阶段诊断](logs/threadfd-vbox-diag.log) |
| QEMU 标准命令e1000实际wire | [补齐测试端后PASS](logs/threadfd-qemu-standard.log)、[断言](logs/threadfd-standard-assertions.log)、[先前缺测试端失败](logs/threadfd-standard-missing-peers.log) |
| 默认Linux关产品shell/native hello/poweroff | [日志](logs/threadfd-default-product.log)、[断言](logs/threadfd-default-product-assertions.log) |

本轮文件：

[threadfd-hosted-on-final.log](logs/threadfd-hosted-on-final.log)、[threadfd-hosted-off-final.log](logs/threadfd-hosted-off-final.log)、[threadfd-hosted-compiled-off.log](logs/threadfd-hosted-compiled-off.log)、[threadfd-hosted-on-assertions.log](logs/threadfd-hosted-on-assertions.log)、[threadfd-hosted-off-assertions.log](logs/threadfd-hosted-off-assertions.log)、[threadfd-hosted-compiled-off-assertions.log](logs/threadfd-hosted-compiled-off-assertions.log)、[threadfd-on-final.log](logs/threadfd-on-final.log)、[threadfd-dual-final.log](logs/threadfd-dual-final.log)、[threadfd-static-final.log](logs/threadfd-static-final.log)、[threadfd-off-final.log](logs/threadfd-off-final.log)、[threadfd-compiled-off-final.log](logs/threadfd-compiled-off-final.log)、[threadfd-on-assertions.log](logs/threadfd-on-assertions.log)、[threadfd-dual-assertions.log](logs/threadfd-dual-assertions.log)、[threadfd-static-assertions.log](logs/threadfd-static-assertions.log)、[threadfd-off-assertions.log](logs/threadfd-off-assertions.log)、[threadfd-compiled-off-assertions.log](logs/threadfd-compiled-off-assertions.log)、[threadfd-vbox-fixed.log](logs/threadfd-vbox-fixed.log)、[threadfd-vbox-repeat.log](logs/threadfd-vbox-repeat.log)、[threadfd-vbox-assertions.log](logs/threadfd-vbox-assertions.log)、[threadfd-vbox-repeat-assertions.log](logs/threadfd-vbox-repeat-assertions.log)、[threadfd-vbox-first-failure.log](logs/threadfd-vbox-first-failure.log)、[threadfd-vbox-diag.log](logs/threadfd-vbox-diag.log)、[threadfd-standard-missing-peers.log](logs/threadfd-standard-missing-peers.log)、[threadfd-qemu-standard.log](logs/threadfd-qemu-standard.log)、[threadfd-standard-assertions.log](logs/threadfd-standard-assertions.log)、[threadfd-default-product.log](logs/threadfd-default-product.log)、[threadfd-default-product-assertions.log](logs/threadfd-default-product-assertions.log)、[threadfd-fixture-final.log](logs/threadfd-fixture-final.log)、[threadfd-kernel-final.log](logs/threadfd-kernel-final.log)、[threadfd-kernel-diagnostics.log](logs/threadfd-kernel-diagnostics.log)、[threadfd-iso-on.log](logs/threadfd-iso-on.log)、[threadfd-build-info.log](logs/threadfd-build-info.log)、[threadfd-iso-product.log](logs/threadfd-iso-product.log)

## 2026-10-17：Linux 单线程受控迁移

实现与禁止点见 [LINUX_MIGRATION.md](../../docs/LINUX_MIGRATION.md)。不是线程组迁移。
以下均为本轮真实运行；hosted 不伪造硬件交接，关闭配置明确 skip。

| 场景 | 证据 |
| --- | --- |
| hosted 开/运行关/编译关 | [开](logs/migrate-hosted-on.log)、[运行关](logs/migrate-hosted-off.log)、[编译关](logs/migrate-hosted-compiled-off.log) |
| QEMU SMP4，Linux/interp 开，e1000 实际 UDP/TCP | [日志](logs/migrate-on-final.log)、[递归断言](logs/migrate-on-assertions.log) |
| QEMU 双节点 SMP4，迁移/COW/commit | [日志](logs/migrate-dual-final.log)、[断言](logs/migrate-dual-assertions.log) |
| QEMU 运行关闭/编译关闭 | [运行关](logs/migrate-off-final.log)、[编译关](logs/migrate-compiled-off-final.log) |
| VBox 7.2.20 UEFI/SMP4/noNIC | [全量](logs/migrate-vbox-final.log)、[断言](logs/migrate-vbox-assertions.log) |
| 默认产品，无 ISO 安装盘，重定向/持久化/nc/http-get/httpd/kill/poweroff | [首次](logs/migrate-product-first.log)、[冷启动](logs/migrate-product-second.log)、[宿主断言](logs/migrate-product-assertions.log) |
| 首轮槽指针误匹配导致超时，保留失败 | [原日志](logs/migrate-qemu.log)、[状态诊断](logs/migrate-qemu-diag.log) |
| 双节点首轮迁移通过、后续 KVM entry failed，保留失败 | [日志](logs/migrate-dual-first-failure.log) |

双节点失败与 VBox/QEMU 同时运行发生在同一测试批次；这里只记录相关性，未断言根因。
停止 VBox 后单独复测全量 PASS；失败日志不计入全绿证据。

## 2026-10-17：静态 MicroPython 黄金展示品

固定MicroPython1.24.1 + 定制musl1.2.5标量构建；不是通用Python/Linux。
范围、哈希、配方及完整矩阵见 [LINUX_MICROPYTHON.md](../../docs/LINUX_MICROPYTHON.md)。

| 场景 | 本轮证据 |
| --- | --- |
| 三个独立干净目录源码重建，字节一致 | [比较](logs/mp-repro-assertions.log)、[源码/工具链/产物摘要](logs/mp-build-summary.log)、[fixture及宿主断言](logs/mp-verify.log) |
| hosted 开/运行关/编译关 | [开](logs/mp-hosted-on.log)、[运行关](logs/mp-hosted-off.log)、[编译关](logs/mp-hosted-compiled-off.log) |
| QEMU SMP4，e1000实际wire，全量 | [单节点](logs/mp-on-final.log)、[递归断言](logs/mp-on-assertions.log) |
| QEMU 双节点/interp关/运行关/编译关，全量 | [双节点](logs/mp-dual-final.log)、[interp关](logs/mp-static-final.log)、[运行关](logs/mp-off-final.log)、[编译关](logs/mp-compiled-off-final.log) |
| VBox7.2.20 UEFI/SMP4/AHCI光驱/noNIC，全量 | [日志](logs/mp-vbox.log)、[断言](logs/mp-vbox-assertions.log) |
| 解释器scope ENOSYS精确差值 | [单节点0](logs/mp-enosys.log)、[双节点0](logs/mp-dual-enosys.log) |
| 无ISO产品盘，Linux开启，两次启动脚本+重定向持久化+原生网络/kill/poweroff | [首次](logs/mp-product-on-first.log)、[冷启动](logs/mp-product-on-second.log)、[断言](logs/mp-product-on-assertions.log) |
| 无ISO产品盘，Linux默认关闭，原生回归 | [首次](logs/mp-product-off-first.log)、[冷启动](logs/mp-product-off-second.log)、[断言](logs/mp-product-off-assertions.log) |

hosted明确skip guest执行；实际脚本/异常/GC/回收由CPL3 guest日志证明。
产品断言不是全量selftest。原生/hush/ldso等既有黄金由递归断言保留。

## 2026-10-18：产品用户旅程（无内核改动）

命令、边界与完整矩阵见 [USER_JOURNEY.md](../../docs/USER_JOURNEY.md)。
QEMU与VBox分别完成三次**无ISO**冷启动：默认原生→离线启用Linux→MicroPython/原生HTTP→
离线关闭Linux→拒绝解释器而保留原生和持久文件。自动化使用PS/2输入，不绕过用户shell。

| 环境 | 本轮证据 |
| --- | --- |
| QEMU KVM/SMP4/AHCI/e1000 | [完整结果](logs/journey-qemu-complete-check.log)、[脚本及HTTP运行](logs/journey-qemu-linux.log)、[HTTP200/25字节/EOF校验](logs/journey-qemu-http-client.log)、[关闭后](logs/journey-qemu-disabled.log) |
| VBox7.2.20 UEFI/SMP4/AHCI/e1000 | [完整结果](logs/journey-vbox-complete-check.log)、[脚本及HTTP运行](logs/journey-vbox-linux.log)、[HTTP客户端](logs/journey-vbox-http-client.log)、[关闭后](logs/journey-vbox-disabled.log) |
| 构建/hosted/默认产品ISO | [产物标识](logs/journey-build-id.log)、[hosted全量](logs/journey-hosted.log)、[ISO提示符烟测](logs/journey-iso-boot.log) |
| 保留失败：VBox状态查询返回1 | [首轮失败](logs/journey-vbox-first-failure.log)、[guest关机记录](logs/journey-vbox-first-native.log) |
| 保留失败：SMP控制台交错导致文本断言过严 | [断言失败](logs/journey-qemu-console-assert-failure.log)、[原始交错](logs/journey-qemu-console-interleaved.log) |

旅程PASS不是全量freestanding selftest；ISO烟测为产品提示符闲置超时，不冒充0 failures。
本轮没有改KPTI/buddy/调度/网络/ABI等内核源码。失败说明与关闭方式见旅程文档。

## Hosts-only stub（2026-10-18）

[接口/限制与环境矩阵](../../docs/HOSTS_RESOLVER.md)。未增加网络 DNS、未修改 TCP/RTO/IRQ。
QEMU 与 VBox 各两次无 ISO 冷启动，原生/Linux HTTP 按 hostname、alias、literal 取得并校验正文，
未知名称 exit=2，hosts 文件冷启动保留：
[QEMU结果](logs/hosts-qemu-result.log)、[VBox结果](logs/hosts-vbox-result.log)。
全量：[hosted开](logs/hosts-hosted-on.log)、[hosted关](logs/hosts-hosted-off.log)、
[QEMU开](logs/hosts-qemu-debug-on.log)、[QEMU关](logs/hosts-qemu-debug-off.log)。
[默认ISO](logs/hosts-product-iso-boot.log)仅提示符闲置烟测。
失败保留并说明关闭方式，见上述文档，未将失败或 skip 当作网络通过。

## 最小 SIGCHLD（2026-10-18）

[语义/限制与矩阵](../../docs/LINUX_SIGCHLD.md)。合并 pending、DFL 可 wait、IGN/NOCLDWAIT 自动回收；
用户 handler 仍拒绝，sigreturn ENOSYS 仍计数一次，不是完整信号框架。
[QEMU开启全量](logs/sigchld-qemu-on.log)、[关闭](logs/sigchld-qemu-off.log)、
[VBox UEFI四核全量](logs/sigchld-vbox-on.log)、[检查](logs/sigchld-vbox-on-check.log)、
[hosted开](logs/sigchld-hosted-on.log)、[hosted关](logs/sigchld-hosted-off.log)。
均有全量0 failures；无NIC的wire项明确skip。hush JOB/TRAP未开启。

## 固定负载性能基线（2026-10-18）

[USER_JOURNEY历史性能记录](../../docs/USER_JOURNEY.md#历史性能记录)。
同一固定负载、1/4 vCPU、512MiB、AHCI无ISO、e1000、Linux=1/interp=0：
[QEMU数字](logs/perf-qemu-numbers.log)、[VBox数字](logs/perf-vbox-numbers.log)、
[QEMU原始秒数/配置](logs/perf-qemu-results.log)、[VBox原始秒数/配置](logs/perf-vbox-results.log)、
[QEMU检查](logs/perf-qemu-summary.log)、[VBox检查](logs/perf-vbox-summary.log)。
HTTP每批20次、预热8次；hush每次32×6000B、预热一次，各测3次并校验全部输出。
性能PASS不等于全量selftest；吞吐包括端到端开销，未关闭日志或调优内核。
原型MicroPython文件校验失败、VBox测前carrier-down失败均保留并在矩阵解释，未计入数字。

## FD64（2026-10-18）

[容量/内存/错误码与矩阵](../../docs/FD_TABLE.md)。原生和Linux共用64槽，新增192B/proc记录、
固定32记录池增加6144B静态元数据；不是扩大VFS/TCP后端或改变Linux默认开关。
[hosted开](logs/fd64-hosted-on.log)、[关](logs/fd64-hosted-off.log)、
[QEMU开](logs/fd64-qemu-on.log)、[关](logs/fd64-qemu-off.log)、
[VBox UEFI四核](logs/fd64-vbox-on.log)均全量0 failures。
[产品httpd/hush](logs/fd64-product-check.log)另验无ISO网络和管道正文。
首轮旧线程ELF32边界导致3 failures、旧检查器挑错第一次fork的失败均保留，见矩阵关闭说明。

## 线程组槽回收复用（2026-10-18）

[生命周期与限制表](../../docs/LINUX_THREADS.md)。仍为含leader最多8个组内记录；
调度上下文回收后，后续clone可移除退出成员旧TID并复用槽，不再累计耗尽。
[hosted开](logs/threadreuse-hosted-on.log)、[关](logs/threadreuse-hosted-off.log)、
[QEMU开](logs/threadreuse-qemu-on.log)、[关](logs/threadreuse-qemu-off.log)、
[双NUMA](logs/threadreuse-numa.log)、[VBox UEFI四核](logs/threadreuse-vbox-on.log)均全量0 failures。
真实guest BSP/AP各32轮clone/join；上下文/fd-owner未释放禁止复用、cap=8仍EAGAIN、页/commit回收均验证。
无网卡wire测试明确skip；hosted不执行ring3。旧NUMA命令配置拒绝日志保留，改memdev后实际通过。

## Linux只读PRIVATE文件映射（2026-10-18）

[接口/commit/fork/矩阵](../../docs/LINUX_FILE_MMAP.md)。逐页预占的驻留快照，可写/shared明确拒绝，
fork只读共享、不额外预占这些只读页的可写副本。
[hosted开](logs/filemmap-hosted-on.log)、[关](logs/filemmap-hosted-off.log)、
[QEMU开](logs/filemmap-qemu-on.log)、[关](logs/filemmap-qemu-off.log)、
[VBox UEFI四核](logs/filemmap-vbox-on.log)均全量0 failures；guest实际open/map/read/close/fork/unmap。
完整检查链含hush、静态/动态/TLS黄金；无网卡wire测试明确skip，hosted不执行ring3。
首轮探针文本段超链接布局的构建失败保留；移入数据段后最终矩阵通过。

## 持socket/pipe受控迁移（2026-10-18）

[禁止点、所有权、锁序与矩阵](../../docs/LINUX_MIGRATION.md)。单线程同任务dup允许，
外部同端点别名/在途额外引用/嵌套syscall拒绝；旧CPU离开后才允许目标CPU续跑，RX队列原位保持。
[hosted开](logs/endpoint-migrate-hosted-on.log)、[关](logs/endpoint-migrate-hosted-off.log)、
[QEMU无NIC开](logs/endpoint-migrate-qemu-on.log)、[关](logs/endpoint-migrate-qemu-off.log)、
[e1000 wire](logs/endpoint-migrate-qemu-e1000.log)、[virtio-net wire](logs/endpoint-migrate-qemu-virtio.log)、
[VBox四核](logs/endpoint-migrate-vbox-on.log)均全量0 failures。
pipe/UDP排队后跨CPU读回；有网卡时真实TCP连接保持，CPU0/1收发payload，注入回滚与端点/页/commit回收通过。
首轮无NIC仍连接echo导致失败、诊断与最外层检查器误传--wire拒绝均保留，未算作PASS。
本轮未另测双NUMA或无ISO产品盘旅程；无NIC的TCP wire明确skip。

## shell控制台前缀协议（2026-10-18）

[协议/限制/矩阵](../../docs/SHELL.md)、[旅程更新](../../docs/USER_JOURNEY.md)。
用户shell用单次console写输出ready/begin/end序号记录，内核日志仍用既有级别前缀；
输入仍逐键回显，不改调度，不实现多tty。脚本排除begin之前的回显再校验结果。
[QEMU旅程](logs/shellproto-qemu-check.log)、[VBox旅程](logs/shellproto-vbox-check.log)均完整三次无ISO冷启动PASS，
含文件/重定向、MicroPython、宿主HTTP正文、禁用Linux保留原生、ACPI关机。
每条返回命令有seq/status boundaries=verified；产品旅程PASS不等于全量selftest。
[hosted](logs/shellproto-hosted.log)另为全量0 failures，PS/2/ring3路径由真实VM旅程覆盖。

本轮另有[QEMU四核debug](logs/shellproto-debug.log)、[完整断言链](logs/shellproto-debug-check.log)全量0 failures；
[默认产品ISO启动](logs/shellproto-product.log)仅到提示符，空闲timeout=124，不冒充全量PASS。

## VBox ICH9 e1000 INTx与原生HTTP量化（2026-10-19）

[路由探测/源码/回退/矩阵](../../docs/VBOX_ICH9_INTX.md)。root slot3/INTA的实际GSI19，firmware line11不直接用作IOAPIC GSI。
未知/冲突路由仍poll-10ms，强制capmix.e1000_irq=0可做同内核对照；RTO和周期推进未改。
[同内核对照](logs/ich9-comparison.log)：Linux关、1/4CPU各3×20HTTP，预热8次，
吞吐中位7.301→8.068及7.517→8.537 req/s；p95为146.743→128.814及136.082→120.229ms。
[IRQ原始JSON](logs/ich9-irq-results.log)、[poll原始JSON](logs/ich9-poll-results.log)保留每请求延迟和内核hash，
性能PASS不代表全量selftest，也不保证任何配置必然提速。
[VBox ICH9 debug](logs/ich9-vbox-debug.log)、[QEMU i440FX](logs/ich9-qemu-e1000.log)、
[Q35 fallback](logs/ich9-qemu-fallback.log)、[virtio/Linux开](logs/ich9-qemu-virtio.log)、
[hosted](logs/ich9-hosted.log)均全量0 failures。
初版平台ID不足导致auto实际fallback的试跑、缺少NAT localhostreachable导致3 failures的debug首轮均保留，未计作成功。
默认产品ISO到shell仅为启动检查，timeout=124不是全量PASS。

## httpd有界独立端口多进程（2026-10-19）

[模型/命令/限制/矩阵](../../docs/HTTP_WORKERS.md)。同一原生httpd由shell启动1/2/4实例，各自listen/accept，
客户端显式按端口分片；不是单端口共享accept，也不新增spawn授权/accepted socket交接ABI。
相同4并发客户端、相同内核/请求数、Linux关，三批×20：
[QEMU对照](logs/httpworkers-qemu-compare.log)四进程1→4CPU中位26.245→31.429 req/s（+19.76%）；
[VBox ICH9对照](logs/httpworkers-vbox-compare.log)26.505→29.974（+13.09%）。
两端四进程p95均变差，未称全部延迟改善；单进程对照/每请求数组/PID-CPU-port均保留。
所有实例配额、HTTP正文/EOF及exit=0/reap核验通过，性能PASS不等于全量selftest。
[旧CLI三次无ISO旅程](logs/httpworkers-journey-check.log)通过；
[hosted](logs/httpworkers-hosted.log)、[QEMU debug](logs/httpworkers-debug.log)另为全量0 failures。
默认产品ISO到shell仅启动检查，timeout=124不冒充全量PASS。

## 屏幕小写与大小写路径（2026-10-19）

[范围/环境矩阵](../../docs/CONSOLE_READABILITY.md)；[hosted 全量](logs/console-case-hosted.log)、[QEMU 全量无 NIC](logs/console-case-debug.log)、[QEMU 产品](logs/console-case-qemu-check.log)、[VBox 产品](logs/console-case-vbox-check.log)。实际屏幕：[QEMU](screenshots/console-case-qemu-case-first.png)、[VBox](screenshots/console-case-vbox-case-first.png)。PNG 保留原始捕获，哈希见[截图记录](logs/console-case-screenshots.log)，不作为 UTF-8 日志条目。

## FazAttrhyiz 更名与 .faz（2026-10-19）

[范围与矩阵](../../docs/FAZATTRHYIZ.md)：[hosted](logs/fazattrhyiz-hosted.log)、[QEMU Linux 全量](logs/fazattrhyiz-linux-debug.log)、[QEMU 产品](logs/fazattrhyiz-qemu-check.log)、[VBox 产品](logs/fazattrhyiz-vbox-check.log)。产品验证 `.faz` 与兼容 `.elf`、HTTP、新提示符及无 ISO 冷启动。历史记录不改名。
