# 从冷启动到脚本和网络：一条可复制的用户旅程

目标是实际产品路径：**无 ISO 冷启动 → 用户态 CapMix shell → 可选离线启用 Linux →
MicroPython → 原生 httpd**。不依赖跑完 phase 自测，不修改内核行为。
默认产品 Linux 关闭；本轮网络主线选原生 httpd，不要求 Linux 服务端程序。

## 0. 准备与边界

从仓库根目录执行。需要已安装的 gcc/binutils/make/Python3、mtools（mcopy/mmd/mdir）、
dosfstools（mkfs.fat）、QEMU/KVM；VBox路径另需VBoxManage和可用UEFI虚拟化环境。
`prepare` 会构建 `LINUX_ABI=1` 的内核及离线配置工具，但安装出的启动配置仍是
**默认 Linux 关闭**。使用仓库钉住的解释器，不临时编译另一份 Python 或下载软件。

只使用新的一次性lab/镜像。安装器继续拒绝未知非空盘、设备、符号链接等；脚本拒绝
覆盖已有lab镜像/标记。不要把示例路径替换为真实系统盘。root布局沿用现有安装方案。

产品输入来自虚拟机窗口的PS/2键盘，FB/串口镜像输出。**串口 stdio 不是交互键盘入口**。
无图形界面时使用下文自动旅程，它仍通过PS/2注入输入，不绕过shell调用内核接口。
不要同时运行QEMU KVM和VBox硬件加速guest；按顺序测试。

## 1. QEMU：默认原生冷启动（无 ISO）

宿主：

```sh
lab=$(mktemp -d /tmp/capmix-journey-qemu.XXXXXX)
scripts/demo-user-journey.sh prepare "$lab"
scripts/demo-user-journey.sh status "$lab"
scripts/demo-user-journey.sh qemu "$lab"
```

`qemu`等价于以下命令，根盘和内核均来自安装盘，没有 `-cdrom`：

```sh
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -drive "id=root,file=$lab/system.img,if=none,format=raw" \
  -device ich9-ahci,id=ahci -device ide-hd,drive=root,bus=ahci.0 \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:18080-:8080 \
  -device e1000,netdev=n0 -serial stdio -display gtk -no-reboot
```

预期关键词：`configured=disabled`、`boot mode: product`、`root=disk`、`capmix> `。
首启会格式化**安装器已划定且仍空白的COW根分区**并provision系统文件；不会把整块
未知数据盘任意格式化。后续冷启动挂载已有根，不重装/覆盖已有同名程序。

在虚拟屏幕键入（以下不是宿主bash命令；不要输入 `capmix>` 前缀）：

```text
help
run /bin/hello.elf
echo journey-native > /journey-native.txt
poweroff
```

预期：帮助列表、`hello from elf`、`exit=0 reap=ok`、
`shell redirect committed: status=0`、`poweroff: ACPI S5 request`，QEMU退出。

**到这里已是一条完整原生路径**。不想启用Linux可用同一 `qemu` 命令再次冷启动，然后转到第3节运行原生httpd。

## 2. 可选 Linux：关机后启用，下次冷启动生效

Linux不是shell内热开关。确认QEMU已退出，再在宿主执行：

```sh
scripts/demo-user-journey.sh linux-on "$lab"
scripts/demo-user-journey.sh status "$lab"
scripts/demo-user-journey.sh qemu "$lab"
```

实际调用既有 `scripts/linux_package.py enable LAB/system.img`。预期：
`enable published: next boot`、`data=retained; native /bin retained; no hot unload`、
`configured=enabled`。这只改安装盘的启动配置；不删除原生程序。
解释器为静态ELF，**不需要 `linux_interp=1`**，工具会保留最小静态配置 `linux_interp=0`。

再次进入guest shell：

```text
run /bin/cat.elf /journey-native.txt
run /bin/micropython.elf /data/linux/micropython-demo.py
run /bin/micropython.elf /data/linux/micropython-demo.py > /journey-python.txt
run /bin/cat.elf /journey-python.txt
```

预期旧文件仍为 `journey-native`，解释器输出：

```text
micropython golden
42
```

之后 `exit=0 reap=ok`；重定向记录 `status=0`。SMP下shell状态与子进程直接stdout的多次write可能交错，
例如CPU编号紧邻 `42`；**以重定向文件的原生cat读回验收完整输出**，不要求混合串口流逐字节连续。
这不改变解释器或控制台实现。init只在Linux开启时补缺解释器与示例。
固定ELF、哈希、独立构建配方见 [LINUX_MICROPYTHON](LINUX_MICROPYTHON.md)。

## 3. 原生 HTTP 服务端：宿主经转发访问 guest

guest shell中运行：

```text
run /bin/httpd.elf 8080 2 &
```

等待 `httpd listening`，再从**另一个宿主终端、仓库目录**运行：

```sh
python3 tests/check_httpd.py --port 18080 --requests 2
```

预期两次 `status=200 payload=25 verified EOF=ok`，最后 `httpd client: PASS`。
响应为 `HTTP/1.0 200 OK`，正文 `hello from CapMix httpd!` 加换行。
guest显示 `httpd handled=2`；可再运行hello或按回车让shell回收后台任务。

这里是宿主 `127.0.0.1:18080` → NAT → guest `10.0.2.15:8080`，**不是guest本机TCP回环**。
服务参数2为有限连接尝试数；校验器消耗两次。之后再curl会失败，须重启服务，
或另开较大次数的实例。不要用额外连接探测先消耗次数。

可选原生nc（QEMU，非本轮主线验收步骤）：宿主先开 `python3 scripts/tcp-echo.py`，
guest执行 `run -i /bin/nc.elf 10.0.2.2 9998`，输入文本后应有 `nc rx bytes=`，
输入 `/quit` 关闭。它连接宿主echo，不是连接guest自身。
VBox宿主回环可达性配置可能不同，优先使用本节已验证的宿主→guest HTTP转发。

## 4. 可选关闭：原生和数据仍保留

guest先 `poweroff`，宿主确认退出：

```sh
scripts/demo-user-journey.sh linux-off "$lab"
scripts/demo-user-journey.sh qemu "$lab"
```

guest：

```text
run /bin/cat.elf /journey-native.txt
run /bin/cat.elf /journey-python.txt
run /bin/micropython.elf /data/linux/micropython-demo.py
run /bin/hello.elf
poweroff
```

预期两份持久文件仍可读；已安装的Linux解释器被拒绝，日志含 `linux persona disabled`
和 `shell error=-7`（NOTSUP）；原生hello仍退出0。若从未启用/安装过解释器，关闭状态
下也可能先表现为文件不存在，不能把它写成“运行成功”。此旅程不使用 `--purge-data`。

## 5. VirtualBox：UEFI + AHCI + e1000，同一旅程

使用独立lab，不能与QEMU共用运行中的盘。以下命令适用于本轮VBox7.2.20；
其他版本选项名可能不同，以本机 `VBoxManage help modifyvm` 为准。
所有路径为绝对路径，VM名称须未被占用。

```sh
vboxlab=$(mktemp -d /tmp/capmix-journey-vbox.XXXXXX)
scripts/demo-user-journey.sh prepare "$vboxlab"
vm=CapMix-User-Journey
VBoxManage createvm --name "$vm" --platform-architecture x86 --ostype Other_64 \
  --basefolder "$vboxlab/vm" --register
VBoxManage modifyvm "$vm" --memory 512 --cpus 4 --ioapic on --acpi on \
  --chipset piix3 --firmware efi64 --graphicscontroller vmsvga --vram 16 \
  --keyboard ps2 --mouse ps2 --nic1 nat --nic-type1 82540EM --cable-connected1 on \
  --boot1 disk --boot2 none --boot3 none --boot4 none \
  --uart1 0x3f8 4 --uart-mode1 file "$vboxlab/native.log" \
  --natpf1 'httpd,tcp,127.0.0.1,18081,10.0.2.15,8080'
VBoxManage storagectl "$vm" --name SATA --add sata --controller IntelAhci --portcount 1
VBoxManage convertfromraw "$vboxlab/system.img" "$vboxlab/native.vdi" --format VDI
VBoxManage storageattach "$vm" --storagectl SATA --port 0 --device 0 \
  --type hdd --medium "$vboxlab/native.vdi"
VBoxManage startvm "$vm" --type gui
```

无ISO/光驱附件。EFI Secure Boot应关闭；固件启动前提见 [VBox指南](VIRTUALBOX_TESTING.zh-CN.md)。
在窗口执行第1节原生命令并关机；确认VBox管理器为Powered Off，再继续。
若关闭瞬间CLI状态查询失败，稍后重试只读查询；不应把 `aborted` 当作正常关机。

**启动后的VDI才是最新数据源**。原始 `system.img` 不会自动接收VDI里的写入；
不能对陈旧RAW启用Linux后直接覆盖VDI，否则会丢掉刚写的文件。正确流程是：

```sh
VBoxManage storageattach "$vm" --storagectl SATA --port 0 --device 0 --type hdd --medium none
VBoxManage clonemedium disk "$vboxlab/native.vdi" "$vboxlab/current-native.img" --format RAW
VBoxManage closemedium disk "$vboxlab/native.vdi"
python3 scripts/linux_package.py enable "$vboxlab/current-native.img"
VBoxManage convertfromraw "$vboxlab/current-native.img" "$vboxlab/linux.vdi" --format VDI
VBoxManage storageattach "$vm" --storagectl SATA --port 0 --device 0 --type hdd --medium "$vboxlab/linux.vdi"
VBoxManage modifyvm "$vm" --uart-mode1 file "$vboxlab/linux.log"
VBoxManage startvm "$vm" --type gui
```

guest执行第2节MicroPython/持久文件命令和第3节httpd命令。宿主校验改用VBox转发端口：

```sh
python3 tests/check_httpd.py --port 18081 --requests 2
```

预期同样 `HTTP200/payload=25/EOF=ok`。如要关闭Linux，再关机，将**最新linux.vdi**
克隆成另一个新RAW，对它执行 `linux_package.py disable`，转成新VDI后挂载冷启动；
操作顺序与启用相同，不能回退到旧native.vdi。默认保留所有数据。

## 6. 一键实测（headless，但仍走真实PS/2/shell）

需要KVM或VBox实际可用，且对应宿主端口空闲。每个后端新建lab：

```sh
qlab=$(mktemp -d /tmp/capmix-journey-autoq.XXXXXX)
scripts/demo-user-journey.sh prepare "$qlab"
CAPMIX_HTTP_PORT=18080 scripts/demo-user-journey.sh check-qemu "$qlab"

# 先停止QEMU再运行VBox，独立盘、独立转发端口
vlab=$(mktemp -d /tmp/capmix-journey-autov.XXXXXX)
scripts/demo-user-journey.sh prepare "$vlab"
CAPMIX_HTTP_PORT=18081 scripts/demo-user-journey.sh check-vbox "$vlab"
```

检查器依次完成三次冷启动，断言配置开关、原生hello、脚本/42/退出0、重定向读回、
HTTP响应、关闭后拒绝Linux而保留原生与数据。VBox自动转换最新VDI，不复用旧RAW；
结束移除自己创建的临时VM，保留lab里的磁盘和日志。失败抛错，不输出旅程PASS；
重跑请用新lab，已有日志/镜像不自动清空。无VBox/无KVM是不可执行，**不是PASS**。

期望末行：`user journey qemu: PASS (...)` / `user journey vbox: PASS (...)`。
这些是**产品旅程断言**，不冒充 `selftest result: PASS (0 failures)`。

## 常见失败与不宣称项

| 现象 | 检查/处理 |
| --- | --- |
| 启动后是phase日志，不见产品shell | 使用默认产品盘配置，勿选 `capmix.mode=selftest`；产品不用等phase跑完 |
| 无法启动/缺KVM权限 | 检查 `/dev/kvm`、宿主虚拟化支持；不要把未启动当作旅程通过 |
| 串口里打字无响应 | 在虚拟屏幕键入；headless用自动PS/2检查器 |
| MicroPython不存在或NOTSUP | 先正常关机；对**最新离线盘**启用Linux再冷启动；旧内核还需先更新到v7支持版本 |
| 仅修改宿主 capmix.elf/ISO，安装盘仍旧行为 | 无ISO启动读取盘上ESP的内核；不会自动更新已安装盘 |
| httpd启动失败/host连接超时 | 检查e1000/virtio网卡、link/up、实际guest IP、转发目标、端口占用；先等 `httpd listening` |
| 无网卡 | 原生/脚本仍可演示；网络段明确无法验收，不把skip写成HTTP PASS |
| 第二个客户端后连接失败 | 有限次数服务已退出；重新启动，不是无限连接服务 |
| 屏上数字/行与shell状态混在一起 | SMP不同进程的多次write可交错；使用 `> /journey-python.txt` 后原生cat读回，完整输出不能用混合串口流的邻接关系判断 |
| VBox数据看似回退 | 以最新VDI导出RAW；不要用第一次转换前的旧system.img代替 |
| VBox关闭时状态查询失败 | 重试只读状态查询；自动检查器保留诊断，有界等待真正poweroff，持久失败仍报错 |

不宣称任意Linux发行版、完整glibc/Python生态、动态版解释器、TLS/HTTPS、DNS、
无限连接、完整POSIX信号/线程、guest本机TCP回环、图形安装器或内核热卸。
双轨限制分别见 [Linux ABI](LINUX_ABI_L5.md)、[解释器](LINUX_MICROPYTHON.md)、
[HTTP服务端](HTTPD.md)、[shell](SHELL.md)。原生新程序 `.cmx`/`/bin/cm/` 约定保持不变。

## 本轮环境与证据

2026-10-18 实际运行，无内核源码/行为修改。QEMU8.2.2/KVM与VBox7.2.20均为
512MiB、4vCPU、AHCI根盘、e1000 NAT，自动PS/2输入，均没有ISO附件。
以下旅程PASS来自真实三次冷启动，不是只检查文件存在或打印预设成功。

| 环境/步骤 | 结果与证据 |
| --- | --- |
| QEMU 三次冷启动全旅程 | [结果](../tests/evidence/logs/journey-qemu-complete-check.log)、[默认原生](../tests/evidence/logs/journey-qemu-native.log)、[Linux/脚本/HTTP](../tests/evidence/logs/journey-qemu-linux.log)、[关闭后原生/数据保留](../tests/evidence/logs/journey-qemu-disabled.log) |
| QEMU 离线开关与真实HTTP响应 | [启用](../tests/evidence/logs/journey-qemu-enable.log)、[关闭](../tests/evidence/logs/journey-qemu-disable.log)、[HTTP两次200/25字节/EOF](../tests/evidence/logs/journey-qemu-http-client.log) |
| VBox UEFI 三次冷启动全旅程 | [结果](../tests/evidence/logs/journey-vbox-complete-check.log)、[默认原生](../tests/evidence/logs/journey-vbox-native.log)、[Linux/脚本/HTTP](../tests/evidence/logs/journey-vbox-linux.log)、[关闭后原生/数据保留](../tests/evidence/logs/journey-vbox-disabled.log) |
| VBox 最新VDI导出、离线开关、真实HTTP | [启用](../tests/evidence/logs/journey-vbox-enable.log)、[关闭](../tests/evidence/logs/journey-vbox-disable.log)、[HTTP响应](../tests/evidence/logs/journey-vbox-http-client.log) |
| 默认产品ISO启动烟测 | [日志](../tests/evidence/logs/journey-iso-boot.log)、[状态说明](../tests/evidence/logs/journey-iso-boot-status.log)。见root=ram和shell提示符，交互闲置30秒超时退出，**不是全量自测PASS** |
| 构建与hosted基线 | [kernel](../tests/evidence/logs/journey-kernel.log)、[ISO](../tests/evidence/logs/journey-iso.log)、[hosted全量0 failures](../tests/evidence/logs/journey-hosted.log)、[构建/脚本标识](../tests/evidence/logs/journey-build-id.log) |

本轮未重跑完整freestanding phase矩阵；v7机制结论仍以既有功能文档/证据为准。
GTK/gui手工命令与headless旅程使用相同盘/网卡配置，**本轮自动验收为headless+PS/2+串口**，
不冒充新增图形截图验收。nc只列为可选步骤，本轮网络证据为原生httpd。

失败记录保留，不计为通过：

- [首轮VBox自动化失败](../tests/evidence/logs/journey-vbox-first-failure.log)：
  [guest已请求ACPI关机](../tests/evidence/logs/journey-vbox-first-native.log)，随后一次
  `showvminfo`返回1；未保留该次CLI详情，不能确定其根因。检查器现有界重试并记录
  只读查询错误，最终完整旅程通过；不是内核修复或把持久失败忽略。
- [QEMU复测文本断言失败](../tests/evidence/logs/journey-qemu-console-assert-failure.log)：
  [原串口交错](../tests/evidence/logs/journey-qemu-console-interleaved.log)显示shell CPU编号
  与解释器两次write之间穿插，子进程仍退出0。检查器改用已有重定向+原生cat验证完整
  输出，最终通过；没有改控制台/调度核心，也不要求混合串口流保持进程级事务性。

