# FazAttrhyiz：系统名称与原生 .faz 后缀

系统名称（代号）为 **FazAttrhyiz**，原名 CapMix。原生软件发布使用 **`.faz`**，代替此前的 `.cmx` 约定。

`.faz` 的内容仍为 x86-64 ELF，使用原生 capability/syscall ABI；不是新容器格式，也不会仅凭扩展名强行选择 persona。Linux ELF 继续用其原有标记与加载器规则识别，Linux 软件仍可保留 `.elf`。原生 ELF 的既有 ABI note、SDK `capmix.h`、`cm_*` 接口及 `capmix.*` 启动参数保持兼容，不因更名改变内核行为。

## 产品使用

```sh
make examples iso
# 新镜像名称；capmix.iso 保留为相同内容的兼容副本
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom fazattrhyiz.iso -serial stdio -display none -no-reboot
```

提示符为 `fazattrhyiz>`。默认 PID1 为 `/sbin/init.faz`，启动 `/bin/shell.faz`。

```text
run /bin/hello.faz
run /bin/ls.faz /bin
run /bin/cat.faz /example.txt
run -i /bin/nc.faz 10.0.2.2 9998
run /bin/http-get.faz 10.0.2.2 8000 /hello.txt
run /bin/httpd.faz 8080 &
```

原生产品工具均提供 `.faz`，包括 shell、spin、hello、filetest、udpwire、tcpwire、argtest、concurrent、hello-sdk、nc、cat、ls、http-get、httpd。旧 `.elf` 路径保留作兼容文件，不删除既有程序、用户数据或 Linux 包。新版本启动只补缺失的文件，已有文件不会被覆盖；因此旧盘需要先更新引导分区的内核，才能携带新版内嵌程序。不会仅靠打开旧盘便升级其内核。

`make examples` 同时生成 `build/example-*.faz` 与兼容 `.elf`；外部程序也可直接将 SDK 编译结果命名为 `hello.faz`，再用 `copy-to-disk.py` 安装到 `/bin/hello.faz`。

磁盘布局、分区标识、FAT 标签、内核文件 `capmix.elf`、安装脚本名及 SDK 路径保留兼容名称，避免已安装盘失去启动能力。历史 evidence 保留原始名称与哈希，不对过去日志改名。旧文档中的 CapMix 指本系统的历史名称；当前产品横幅、启动菜单、shell 和 uname 显示新名称。

## 验收（2026-10-19）

| 环境 | 结果与证据 |
|---|---|
| hosted 全量 | [PASS (0 failures)](../tests/evidence/logs/fazattrhyiz-hosted.log) |
| QEMU/KVM，4 CPU，Linux+interp 开，无 NIC | [全量 PASS (0 failures)](../tests/evidence/logs/fazattrhyiz-linux-debug.log)；外部网络按既有逻辑 skip |
| QEMU/KVM，AHCI+e1000，4 CPU，Linux 默认关 | [产品 PASS](../tests/evidence/logs/fazattrhyiz-qemu-check.log)：两次无 ISO 冷启动、`.faz` hello/cat/httpd、旧 `.elf`、重定向持久化、PS/2 Shift/Caps、ACPI关机；[首次日志](../tests/evidence/logs/fazattrhyiz-qemu-case-first.log)、[冷启动](../tests/evidence/logs/fazattrhyiz-qemu-case-second.log)、[HTTP 客户端校验](../tests/evidence/logs/fazattrhyiz-qemu-case-first-http-client.log) |
| VBox UEFI，AHCI+e1000，4 CPU，Linux 默认关 | [产品 PASS](../tests/evidence/logs/fazattrhyiz-vbox-check.log)：同上；[首次日志](../tests/evidence/logs/fazattrhyiz-vbox-case-first.log)、[冷启动](../tests/evidence/logs/fazattrhyiz-vbox-case-second.log)、[HTTP 客户端校验](../tests/evidence/logs/fazattrhyiz-vbox-case-first-http-client.log) |

[kernel/examples 编译](../tests/evidence/logs/fazattrhyiz-build.log)无 warning/error；[产品 ISO 构建](../tests/evidence/logs/fazattrhyiz-product-iso.log)成功。当前 `fazattrhyiz.iso` 与 `capmix.iso` 字节相同。实际屏幕：[QEMU](../tests/evidence/screenshots/fazattrhyiz-qemu.png)、[VBox](../tests/evidence/screenshots/fazattrhyiz-vbox.png)。产品定向测试不代表 VBox 全量 debug 回归。
