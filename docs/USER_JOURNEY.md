# FazAttrhyiz 用户旅程：从启动 ISO 到安装盘、文件和网络

更新：2026-10-19。按顺序操作即可；第一次建议先走 **QEMU 第1～8步**，再尝试可选功能。
使用 VirtualBox 的读者先完成第1、2步，再转到“VirtualBox 路线”。

**先分清两种命令：**

- 标为“宿主终端”的 `bash` 代码块，在运行虚拟机的 Linux 电脑上执行，通常位于仓库根目录。
- 标为“FazAttrhyiz 虚拟机内”的 `text` 代码块，在虚拟机窗口里键入。不要输入 `fazattrhyiz>` 提示符。
- 点击虚拟机窗口后再打字：输入来自PS/2键盘。串口终端用于看日志，**不是shell键盘入口**。

当前安装方式是“**宿主写入引导镜像 → 首次从盘启动初始化系统根**”。
ISO里没有 `install` 命令或图形安装器；不要在FazAttrhyiz shell里输入宿主的make/Python命令。
默认产品启动进入用户态init/shell，Linux persona默认关闭，不需要先跑完自测。

## 第1步：进入仓库并准备工具

**宿主终端：**

```bash
cd /你的实际路径/capmix
export CAPMIX_LAB="$(mktemp -d /tmp/capmix-user.XXXXXX)"
printf '本次测试目录：%s\n' "$CAPMIX_LAB"
```

把第一行换成仓库实际位置。后面的QEMU步骤尽量在**同一个终端**执行，保留 `CAPMIX_LAB` 变量。
若关了终端，重新进入仓库并将该变量设回刚才打印的目录；不要误指向别的磁盘。

需要gcc/binutils、make、Python3、xorriso、mtools（mcopy/mmd/mdir）、dosfstools（mkfs.fat）。
QEMU路线需要qemu-system-x86_64与可用KVM；VBox路线另需VBoxManage。
以下以Linux x86-64宿主为例；Windows宿主可在Linux构建机生成ISO/磁盘，再复制到VBox使用。

## 第2步：构建产品 ISO

**宿主终端：**

```bash
make kernel
make iso
```

两条都应成功，仓库根目录应出现 `capmix.elf` 和 `capmix.iso`。
这里不要加selftest配置：默认ISO是产品shell，debug ISO留到最后单独验证。
默认构建包含可选Linux转译支持，但产品配置仍关闭persona；若你自行用LINUX_ABI=0构建，后续enable会被拒绝，不能靠改开关凭空补齐内核支持。
已有构建错误时先解决错误，不要继续用之前留下的旧ISO。

## 第3步：先启动 ISO，看见 shell

这一轮先不接硬盘，确认屏幕和键盘能用。

**宿主终端：**

```bash
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom "$PWD/capmix.iso" -boot order=d -net none \
  -serial "file:$CAPMIX_LAB/iso.log" -display gtk -no-reboot
```

这条命令会打开虚拟机窗口。看到以下内容即可继续：

```text
init: FazAttrhyiz product root=ram ...
shell ELF ready: CPL=3 ...
[SHELL v=1 event=ready seq=1 status=0]
fazattrhyiz>
```

无硬盘时 `root=ram` 正常。本轮写文件不会跨关机保存。
点击虚拟屏幕，然后在 **FazAttrhyiz虚拟机内**输入：

```text
help
run /bin/hello.faz
poweroff
```

预期看到帮助、`hello from elf`、`exit=0 reap=ok`，最后ACPI关机，QEMU退出。
不要用 `exit` 代替关机：它只结束shell，不是poweroff。
无ACPI环境会说明降级并安全halt，此时确认已停止执行后再关闭VM窗口。

## 第4步：在宿主安装到一块新磁盘镜像

**先确认上一步QEMU已经退出。**

**宿主终端：**

```bash
make install-disk DISK_IMAGE="$CAPMIX_LAB/system.img"
```

工具会新建默认128MiB镜像，写入Limine引导、FAT ESP、内核和产品启动配置。
预期输出包含：

```text
install boot staged: .../system.img ...
First boot from this disk (no ISO) initializes the blank COW root ...
```

此时只完成了引导准备，COW根会在首次从盘启动时初始化。
不需要另行运行qemu-img创建磁盘，也不需要把ISO文件复制进磁盘。

**不要重复执行安装命令来“更新”已安装盘。** 工具拒绝未知非空盘和已安装盘；没有force重装模式。
本指南只写新建普通镜像文件，不使用真实 `/dev/...` 磁盘。
布局、拒绝规则和升级限制详见 [INSTALL_DISK](INSTALL_DISK.md)。

## 第5步：去掉 ISO，第一次从磁盘启动

下面定义一个启动函数，后面重启同一个盘时直接调用。它**没有 `-cdrom`**，内核与根均来自安装盘。
同时配置e1000和HTTP端口转发，后面可测试网络。

**宿主终端：**

```bash
capmix_disk() {
  qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
    -drive "id=root,file=$CAPMIX_LAB/system.img,if=none,format=raw" \
    -device ich9-ahci,id=ahci -device ide-hd,drive=root,bus=ahci.0 \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:18080-:8080 \
    -device e1000,netdev=n0 \
    -serial "file:$CAPMIX_LAB/disk-current.log" -display gtk -no-reboot
}
capmix_disk
```

这是QEMU默认固件的从盘引导路径；要单独测试UEFI，使用 [INSTALL_DISK的OVMF步骤](INSTALL_DISK.md)。
首次从盘启动应有 `disk install: system ELF provisioning synced`、`root=disk`、用户态init/shell和提示符。
第一次初始化期间稍等，不要强制关机。

**FazAttrhyiz虚拟机内：**

```text
run /bin/hello.faz
run /bin/argtest.faz disk firstboot
echo first-boot-data > /note.txt
run /bin/cat.faz /note.txt
poweroff
```

预期hello/argv程序退出0，cat显示 `first-boot-data`，重定向有 `status=0`，然后正常关机。
这一轮写入的 `/note.txt` 在COW磁盘根中，不在ISO或RAM根中。

## 第6步：第二次冷启动，确认文件还在

**宿主终端，QEMU退出后：**

```bash
cp "$CAPMIX_LAB/disk-current.log" "$CAPMIX_LAB/disk-first.log"
capmix_disk
```

这是重新创建VM进程的冷启动，不是仅在guest里reset。
应继续显示 `root=disk`，挂载已有COW根，而不是重新格式化。

**FazAttrhyiz虚拟机内：**

```text
run /bin/cat.faz /note.txt
echo second-boot-data >> /note.txt
run /bin/cat.faz < /note.txt
run /bin/ls.faz /bin > /bin-list.txt
run /bin/cat.faz /bin-list.txt
```

应先读到上次的 `first-boot-data`，追加后读到两行。`>` 覆盖、`>>` 追加、`<` 作为输入；
这是最小shell语法，不要套用完整bash的引号、变量展开或复杂管道语法。
详见 [SHELL](SHELL.md)。先不要关机，继续验证HTTP。

## 第7步：在FazAttrhyiz启动HTTP服务，宿主访问

**FazAttrhyiz虚拟机内：**

```text
run /bin/httpd.faz 8080 2 &
```

看到 `httpd listening` 后，打开**另一个宿主终端**，进入同一仓库：

```bash
python3 tests/check_httpd.py --port 18080 --requests 2
```

预期两次 `status=200 payload=25 verified EOF=ok`，最后 `httpd client: PASS`。
FazAttrhyiz侧应有 `httpd handled=2`。按一次回车或再运行hello，让shell显示后台 `exit=0 reap=ok`。

转发方向是 **宿主127.0.0.1:18080 → guest 10.0.2.15:8080**，不是guest本机TCP回环。
参数2表示服务两次后退出；不要先用浏览器/curl探测消耗这两次，再运行校验器。
测试完如需再访问，重新启动该服务。

### 可选：让FazAttrhyiz访问宿主HTTP服务

**另一个宿主终端：**

```bash
mkdir -p /tmp/capmix-www
printf 'hello from host\n' > /tmp/capmix-www/hello.txt
python3 -m http.server 8000 --bind 127.0.0.1 --directory /tmp/capmix-www
```

保持此终端运行，在 **FazAttrhyiz虚拟机内**输入：

```text
run /bin/http-get.faz 10.0.2.2 8000 /hello.txt
```

预期HTTP200、正文 `hello from host`、退出0。结束后宿主服务终端可按Ctrl+C。
这里只测试HTTP明文，不是HTTPS；也不是完整DNS解析。

## 第8步：试一下后台进程、kill和关机

**FazAttrhyiz虚拟机内：**

```text
run /bin/spin.faz &
```

记下实际输出的 `shell background start: pid=...`。假如PID是7，再输入：

```text
kill 7
run /bin/hello.faz
poweroff
```

**7只是示例，必须换成刚才的真实PID。** 预期kill accepted、后台被结束/回收，hello正常，最后关机。
poweroff有后台等待上限与超时处理；不是无限等待所有应用自然结束。
若你只想体验原生系统，完成到这里即可，Linux不是必选组件。

## 第9步（可选）：关机后启用Linux，再跑MicroPython

必须确认虚拟机已经关机，安装盘未被QEMU/VBox占用。

**宿主终端：**

```bash
make build/linux-package
python3 scripts/linux_package.py enable "$CAPMIX_LAB/system.img"
python3 scripts/linux_package.py status "$CAPMIX_LAB/system.img"
capmix_disk
```

预期宿主显示 `configured=enabled`，下次启动生效。产品内核仍是双轨：原生 `/bin` 不被替换。
静态解释器不需要开启动态解释器开关；此路径使用 `linux_interp=0`。

**FazAttrhyiz虚拟机内：**

```text
run /bin/cat.faz /note.txt
run /bin/micropython.elf /data/linux/micropython-demo.py
run /bin/micropython.elf /data/linux/micropython-demo.py > /python-output.txt
run /bin/cat.faz /python-output.txt
poweroff
```

应保留之前文件，并读到：

```text
micropython golden
42
```

以重定向文件读回验证完整正文，避免把多个进程的串口write交错误认为内容错误。
仅承诺钉死的解释器黄金用例，不是任意Linux/Python包生态；见 [LINUX_MICROPYTHON](LINUX_MICROPYTHON.md)。

## 第10步（可选）：关闭Linux，确认原生和数据保留

**宿主终端，关机后：**

```bash
python3 scripts/linux_package.py disable "$CAPMIX_LAB/system.img"
python3 scripts/linux_package.py status "$CAPMIX_LAB/system.img"
capmix_disk
```

**FazAttrhyiz虚拟机内：**

```text
run /bin/cat.faz /note.txt
run /bin/cat.faz /python-output.txt
run /bin/micropython.elf /data/linux/micropython-demo.py
run /bin/hello.faz
poweroff
```

两个文件应仍在，MicroPython被拒绝（可见 `linux persona disabled` / `shell error=-7`），原生hello仍退出0。
这里是下次冷启动关闭persona，不是内核热卸。默认不删除 `/data/linux`；本入门流程不使用删除数据选项。

## VirtualBox路线：同样先ISO、再安装盘

以下CLI同样面向Linux宿主；也可按对应说明在GUI操作。
**先停止QEMU/KVM，再运行VBox；不要同时运行两种硬件加速VM。**

### V1：创建一台测试VM，先挂产品ISO

在第1、2步的宿主终端继续执行，VM名必须尚不存在：

```bash
export CAPMIX_VM=FazAttrhyiz-User-Journey
export CAPMIX_VBOX="$CAPMIX_LAB/vbox"
mkdir -p "$CAPMIX_VBOX"
VBoxManage createvm --name "$CAPMIX_VM" --ostype Other_64 --register
VBoxManage modifyvm "$CAPMIX_VM" --memory 512 --cpus 4 --ioapic on --acpi on \
  --chipset ich9 --firmware efi64 --graphicscontroller vmsvga --vram 16 \
  --accelerate-3d off --keyboard ps2 --mouse ps2 \
  --nic1 nat --nic-type1 82540EM --cable-connected1 on --nat-localhostreachable1 on \
  --boot1 dvd --boot2 disk --boot3 none --boot4 none \
  --uart1 0x3f8 4 --uart-mode1 file "$CAPMIX_VBOX/iso.log" \
  --natpf1 'httpd,tcp,127.0.0.1,18080,10.0.2.15,8080'
VBoxManage storagectl "$CAPMIX_VM" --name SATA --add sata --controller IntelAhci --portcount 2
VBoxManage storageattach "$CAPMIX_VM" --storagectl SATA --port 1 --device 0 \
  --type dvddrive --medium "$PWD/capmix.iso"
VBoxManage startvm "$CAPMIX_VM" --type gui
```

GUI对应：Other 64-bit、512MiB/4CPU、UEFI且Secure Boot关闭、ICH9/I/O APIC、
VMSVGA且无3D、PS/2键盘、SATA/AHCI、NAT+Intel PRO/1000 MT Desktop。
不是PCnet、USB键盘或NVMe。不要重复创建已存在的VM。

点击虚拟屏幕，输入 `help`、`run /bin/hello.faz`、`poweroff`。确认VBox为Powered Off。
默认释放键盘常为右Ctrl，以VBox窗口显示的Host键为准。

### V2：在宿主生成系统盘，再转换为VDI

虚拟机保持关机：

```bash
make install-disk DISK_IMAGE="$CAPMIX_VBOX/system.img"
VBoxManage convertfromraw "$CAPMIX_VBOX/system.img" "$CAPMIX_VBOX/system.vdi" --format VDI
```

这里的system.img是**新盘**，与QEMU路线的system.img不是同一个路径。
VBox不会从ISO自动安装；引导内容已经由宿主工具写入新盘。

### V3：挂系统盘、卸下ISO，从盘启动

```bash
VBoxManage storageattach "$CAPMIX_VM" --storagectl SATA --port 0 --device 0 \
  --type hdd --medium "$CAPMIX_VBOX/system.vdi"
VBoxManage storageattach "$CAPMIX_VM" --storagectl SATA --port 1 --device 0 --type dvddrive --medium none
VBoxManage modifyvm "$CAPMIX_VM" --boot1 disk --boot2 none \
  --uart-mode1 file "$CAPMIX_VBOX/disk-first.log"
VBoxManage startvm "$CAPMIX_VM" --type gui
```

应看到首次provision、`root=disk backend=ahci` 和shell。若停在EFI菜单，先确认盘挂在SATA且ISO已卸下，
可从Boot Manager选择磁盘的 `EFI/BOOT/BOOTX64.EFI`；不要把未知固件问题写成安装成功。

**FazAttrhyiz虚拟机内：**

```text
echo vbox-persistent > /vbox-note.txt
run /bin/cat.faz /vbox-note.txt
poweroff
```

### V4：第二次冷启动，读回并测试网络

确认Powered Off后：

```bash
VBoxManage modifyvm "$CAPMIX_VM" --uart-mode1 file "$CAPMIX_VBOX/disk-second.log"
VBoxManage startvm "$CAPMIX_VM" --type gui
```

在FazAttrhyiz内运行 `run /bin/cat.faz /vbox-note.txt`，应读到 `vbox-persistent`。
再按第7步启动 `run /bin/httpd.faz 8080 2 &`，宿主运行相同 `check_httpd.py` 命令。
V1已经添加18080→8080转发；测试时不能另开占用同端口的QEMU。
若要运行第7步的http-get，V1的localhostreachable也已配置；载波up前可稍等几秒，不能只凭up日志声称payload通过。

### V5（可选）：对最新VDI离线启用Linux

先在FazAttrhyiz内 `poweroff`，确认VBox为Powered Off。
**VBox实际写入的是VDI，不能拿V2转换前的旧system.img改配置，否则会回到旧数据。**
导出当前盘、修改配置，再转换成一个新VDI：

```bash
make build/linux-package
VBoxManage storageattach "$CAPMIX_VM" --storagectl SATA --port 0 --device 0 --type hdd --medium none
VBoxManage clonemedium disk "$CAPMIX_VBOX/system.vdi" "$CAPMIX_VBOX/current.img" --format RAW
python3 scripts/linux_package.py enable "$CAPMIX_VBOX/current.img"
VBoxManage convertfromraw "$CAPMIX_VBOX/current.img" "$CAPMIX_VBOX/linux-on.vdi" --format VDI
VBoxManage storageattach "$CAPMIX_VM" --storagectl SATA --port 0 --device 0 --type hdd --medium "$CAPMIX_VBOX/linux-on.vdi"
VBoxManage modifyvm "$CAPMIX_VM" --uart-mode1 file "$CAPMIX_VBOX/linux-on.log"
VBoxManage startvm "$CAPMIX_VM" --type gui
```

进入shell后，在 **FazAttrhyiz虚拟机内**输入：

```text
run /bin/cat.faz /vbox-note.txt
run /bin/micropython.elf /data/linux/micropython-demo.py > /vbox-python.txt
run /bin/cat.faz /vbox-python.txt
poweroff
```

应保留vbox-note，并读到 `micropython golden` 和 `42`。
如需关闭Linux，确认Powered Off，并从**最新linux-on.vdi**导出到新的RAW，再运行disable，转成新的VDI并挂回：

```bash
VBoxManage storageattach "$CAPMIX_VM" --storagectl SATA --port 0 --device 0 --type hdd --medium none
VBoxManage clonemedium disk "$CAPMIX_VBOX/linux-on.vdi" "$CAPMIX_VBOX/current-off.img" --format RAW
python3 scripts/linux_package.py disable "$CAPMIX_VBOX/current-off.img"
VBoxManage convertfromraw "$CAPMIX_VBOX/current-off.img" "$CAPMIX_VBOX/linux-off.vdi" --format VDI
VBoxManage storageattach "$CAPMIX_VM" --storagectl SATA --port 0 --device 0 --type hdd --medium "$CAPMIX_VBOX/linux-off.vdi"
VBoxManage modifyvm "$CAPMIX_VM" --uart-mode1 file "$CAPMIX_VBOX/linux-off.log"
VBoxManage startvm "$CAPMIX_VM" --type gui
```

关闭后的FazAttrhyiz内验证：

```text
run /bin/cat.faz /vbox-note.txt
run /bin/cat.faz /vbox-python.txt
run /bin/micropython.elf /data/linux/micropython-demo.py
run /bin/hello.faz
poweroff
```

文件应保留、MicroPython应被拒绝、原生hello仍能运行。
这些目标文件名仅供第一次操作；重复导出/转换需用新名字，保留旧盘作为备份，不要强制覆盖。
VBox版本差异和更多故障处理见 [VIRTUALBOX_TESTING](VIRTUALBOX_TESTING.zh-CN.md)。

## 不想手工输入？运行完整自动旅程

它仍使用PS/2注入驱动真实shell，不绕过产品入口。需要新目录，不能复用上面已经安装的手工盘：

```bash
scripts/demo-user-journey.sh prepare /tmp/capmix-auto-qemu-01
scripts/demo-user-journey.sh check-qemu /tmp/capmix-auto-qemu-01
# 等QEMU完全退出，再测VBox
scripts/demo-user-journey.sh prepare /tmp/capmix-auto-vbox-01
scripts/demo-user-journey.sh check-vbox /tmp/capmix-auto-vbox-01
```

自动路线执行三次无ISO冷启动：原生写盘→离线启用Linux/MicroPython/HTTP→关闭Linux保留原生与数据。
看到 `user journey qemu: PASS` 或 `user journey vbox: PASS` 才算该旅程通过。
脚本用 `[SHELL v=1 event=ready/begin/end seq=... status=...]` 同步，不把键盘回显中的路径当成文件正文。
协议及关机无end记录的规则见 [SHELL](SHELL.md)。

## 进阶：多进程HTTP、性能和debug

先完成基本旅程，再选择需要的测试，避免把基准命令误当成日常安装命令。

- **httpd多进程**：同一个二进制启动1/2/4个独立端口实例，客户端分片请求；不是单端口共享accept。
  用法、转发和SMP数字见 [HTTP_WORKERS](HTTP_WORKERS.md)。
- **吞吐与延迟**：`scripts/perf-baseline.py`；原生HTTP选 `--http-only`，保持Linux关闭。
  VBox ICH9 IRQ/轮询A/B命令、req/s与p50/p95见 [VBOX_ICH9_INTX](VBOX_ICH9_INTX.md)。
- **外部原生程序**：用宿主GCC和FazAttrhyiz SDK构建，关机后导入盘；见 [EXTERNAL_ELF](EXTERNAL_ELF.md)。
- **hosts解析/nc**：先按 [EXAMPLES](EXAMPLES.md) 启动宿主服务；nc需要 `run -i` 委托交互控制台。

全量自测使用独立debug ISO，先关掉产品VM，不接你的日常系统盘：

```bash
make test
make iso LIMINE_CONFIG=boot/limine-selftest.conf
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display none -no-reboot
# 测完恢复默认产品ISO
make iso
```

debug预期 `selftest result: PASS (0 failures)`。无设备/无VMX等明确skip，不代表硬件功能已验证。
产品旅程PASS与全量selftest PASS是不同口径；不要用产品提示符代替自测结果。

## 常见卡点：停在哪一步就检查哪一项

| 现象 | 先检查 |
|---|---|
| 构建失败却还能开机 | 是否用了旧capmix.iso；先让make成功 |
| KVM不可用 | 宿主硬件虚拟化/权限；不要同时运行VBox与KVM |
| 串口里打字没有反应 | 点击虚拟屏，用PS/2键盘；串口是日志输出 |
| root=ram、关机后文件没了 | 仍在无盘ISO路线；确认启动了安装盘且无-cdrom |
| 安装器拒绝system.img | 文件已非空/已安装；不要重装，创建另一个新路径 |
| 从盘启动失败 | 是否执行过宿主安装工具、挂SATA/AHCI、卸ISO；UEFI需关闭Secure Boot |
| 第二次启动数据回退（VBox） | 是否挂回了旧VDI或旧RAW；以最新写入的VDI为准 |
| HTTP连接被拒绝 | 服务是否仍运行、两次配额是否已用完、18080转发是否存在/被其他VM占用 |
| guest不能访问宿主服务（VBox） | 宿主服务是否监听、localhostreachable是否on；不是入站端口转发的问题 |
| Linux程序被拒绝 | 离线enable后是否真正冷启动，status是否enabled；默认关闭是预期 |
| MicroPython正文被日志插开 | 重定向到文件再cat核对；不要求混合串口流中多次write连续 |
| 重建ISO但盘里程序仍旧 | 安装盘不自动随宿主构建升级；已有同名文件不静默覆盖，勿重跑blank-only安装器 |

## 已有验证记录与范围

本次为操作文档重写，命令对照当前仓库实现；**未因改文档重新启动VM或宣称新测试PASS**。
下表链接到已有真实记录，不把手工步骤的每个可选分支都说成已复测。

| 记录 | 证据与范围 |
|---|---|
| QEMU三次无ISO旅程（2026-10-19） | [检查](../tests/evidence/logs/httpworkers-journey-check.log)：旧httpd CLI、文件持久化/重定向、MicroPython开关、HTTP正文、关机 |
| VBox三次无ISO旅程（2026-10-18） | [检查](../tests/evidence/logs/shellproto-vbox-check.log)：UEFI/AHCI/e1000 NAT、PS/2、shell协议、原生/Linux切换；历史自动配置为PIIX3 |
| VBox ICH9产品盘/IRQ（2026-10-19） | [IRQ四核串口](../tests/evidence/logs/ich9-irq-smp4.log)、[对照](../tests/evidence/logs/ich9-comparison.log)：无ISO原生HTTP性能/实际IRQ，非完整手工旅程 |
| 多进程httpd（2026-10-19） | [QEMU](../tests/evidence/logs/httpworkers-qemu-compare.log)、[VBox ICH9](../tests/evidence/logs/httpworkers-vbox-compare.log)：1/4CPU、独立端口、配额/退出/reap；p95未全面改善 |
| 全量debug与hosted（2026-10-19） | [QEMU](../tests/evidence/logs/httpworkers-debug.log)、[hosted](../tests/evidence/logs/httpworkers-hosted.log)：0 failures；debug无NIC wire明确skip |
| 早期完整旅程档案 | [QEMU](../tests/evidence/logs/journey-qemu-complete-check.log)、[VBox](../tests/evidence/logs/journey-vbox-complete-check.log)，详细失败说明见[evidence索引](../tests/evidence/README.md) |

不宣称完整Linux/glibc发行版兼容、任意Python生态、TLS/HTTPS、完整POSIX、自动安装器、通用GPU/USB/NVMe或内核热卸。


### 历史性能记录

保留2026-10-18固定负载记录供查证：Linux=1/interp=0、单进程httpd、串行客户端，
512MiB、AHCI无ISO、e1000，HTTP三批×20、预热8次；hush每样本32×6000B。
与当前Linux关闭/并发客户端/多端口worker配置不同，不能直接归因比较。

| 历史环境 | vCPU | HTTP req/s中位 | hush KiB/s中位 | 证据 |
|---|---:|---:|---:|---|
| QEMU KVM | 1 | 8.013 | 26.51 | [汇总](../tests/evidence/logs/perf-qemu-summary.log)、[原始样本](../tests/evidence/logs/perf-qemu-results.log) |
| QEMU KVM | 4 | 8.100 | 73.44 | 同上 |
| VBox PIIX3 | 1 | 8.604 | 30.89 | [汇总](../tests/evidence/logs/perf-vbox-summary.log)、[原始样本](../tests/evidence/logs/perf-vbox-results.log) |
| VBox PIIX3 | 4 | 8.494 | 41.19 | 同上 |

当前脚本复现相同负载参数可用 `--workers 1 --concurrency 1 --samples 3 --requests 20 --pipes 32`，
不要加http-only（hush需要Linux）；新内核/新shell上的实测数值不承诺等于历史数字。

## 屏幕文字大小写

屏幕现在原样显示 ASCII 大小写，默认 PS/2 输入小写。路径区分大小写，`/bin` 与 `/Bin` 不等价；不会自动转换文件名。详见[控制台可读性与屏幕截图](CONSOLE_READABILITY.md)。
