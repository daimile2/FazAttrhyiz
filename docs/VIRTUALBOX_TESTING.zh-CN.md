# 在 VirtualBox 测试 CapMix 已有功能

更新：2026-10-15。目标是 x86_64 CapMix 的产品体验、磁盘持久化和 debug 回归；
不是安装 Linux，也不依赖 Guest Additions。命令示例使用 Linux 宿主 Bash，仓库根执行。
Windows 宿主可用 GUI 做同样配置，在 Linux 构建机生成 ISO/磁盘后复制过去。

本手册最初整理时核对了源码、阶段记录和本机 `VBoxManage 7.2.20` 参数；
后续已用独立VM完成UEFI/AHCI/4 CPU的重定向写盘与无ISO冷启动实测，
配置、日志和明确的测试范围见 [SHELL环境矩阵](SHELL.md)。这不是VBox全量debug PASS声明。已有VBox定时器测试
记录见 [P0b](P0B_TIMER.md)；QEMU 的网络/VMX PASS 不能直接替代 VBox 验收。

## 1. 测试顺序与覆盖范围

建议按顺序测试，每一步保留独立串口日志：

1. **产品 ISO，无硬盘**：FB、PS/2、用户 init/shell、hello/argv；根为 RAM。
2. **SATA 数据盘**：用户文件 syscall、同步关机、冷启动后读回。
3. **NAT 网卡**：DHCP、UDP、TCP、nc、http-get。
4. **安装系统盘，无 ISO**：从 ESP 引导，COW 根里的 init/shell 和应用可运行。
5. **debug ISO，4 CPU**：buddy soak、时钟、抢占、并发、IPC、TRACE、签名、驱动隔离、VMX/EPT等。
6. **可选负例**：无网卡、DHCP超时回退、死循环后台kill、无VMX skip。

| 已有功能 | VBox 上如何验证 | 不应误判 |
| --- | --- | --- |
| FB/PS2/用户init与shell | 屏幕日志、键盘回显、`init ELF`、`shell ELF ready: CPL=3` | 串口能看日志不等于串口能输入 |
| ELF/argv/文件 | shell run 示例和filetest，跨两次冷启动 | CapMix SDK ELF，不是任意Linux静态程序 |
| 并发/kill/关机 | run&、spin、kill PID、有限等待poweroff | 不是完整POSIX信号或作业控制 |
| UDP/TCP/DHCP/HTTP | NAT+宿主服务，检查payload/退出码 | link up或DHCP ACK不等于应用payload已验证 |
| 网卡RX IRQ | 看驱动的实际enabled/skipped日志 | 当前只对已验证i440FX启用INTx；VBox一般走10ms fallback，不要求虚构IRQ通过 |
| buddy/时钟/压力/COW | 4 CPU debug suite，检查具体断言和总PASS | hosted或1 CPU不能证明4 CPU soak |
| capability/VMO/TRACE/签名/用户驱动 | debug suite，granted/denied、共享内容和隔离日志 | 不代表普通shell可直接操作全部内核接口 |
| VMX/EPT | 默认无nested时明确skip；可选单独开启nested再核对真实exit | guest看不到VMX时skip不等于真实launch通过 |
| NUMA | VBox常见配置检验node0回退 | 不能凭4 vCPU声称多节点SRAT/SLIT测试完成，多节点继续用QEMU专门配置 |

## 2. 构建并区分镜像

宿主需要 GCC/make/Python3、xorriso、Limine构建所需工具；安装盘另需
`mkfs.fat`和mtools。先确认VirtualBox能够运行x86_64来宾，宿主已开放硬件虚拟化。
ARM宿主、嵌套宿主环境和受限硬件不是本手册默认测试环境。

```bash
make test
make kernel 2>&1 | tee /tmp/capmix-vbox-build.log
# 查看警告/错误，同时检查上一步make是否成功，不能只看grep退出码：
grep -iE 'warn|error' /tmp/capmix-vbox-build.log

make examples iso
cp capmix.iso capmix-product.iso
make iso LIMINE_CONFIG=boot/limine-selftest.conf
cp capmix.iso capmix-selftest.iso
make iso                            # 恢复默认产品capmix.iso
```

`capmix-product.iso`进入产品shell；`capmix-selftest.iso`才运行全量自测。
文件名是人工保存的副本，不是Makefile里的新目标。每次改代码后重新构建/复制，
不要误用旧ISO；已安装盘中的内核不会随宿主ISO重建自动更新。

## 3. 新建 VM：推荐配置

使用专用测试VM与新建测试盘。先不接硬盘，先关闭网卡完成最小启动。

| 项目 | 推荐配置 | 原因 |
| --- | --- | --- |
| 客户机类型 | Other / Other 64-bit | 不需要自动安装其他OS |
| 内存/CPU | 512MiB或1GiB，4 vCPU | 4 CPU才覆盖buddy并发与逐CPU时钟断言；内核不保证用尽全部RAM |
| 固件 | EFI 64-bit，关闭Secure Boot | 使用GOP/EFI fallback；CapMix无签名启动链 |
| 主板 | PIIX3，启用I/O APIC、ACPI | 作为统一测试基线，不代表VBox等于QEMU i440FX |
| 显示 | VMSVGA，16MiB显存，关闭3D | 只用线性FB文本，不装Guest Additions |
| 输入 | PS/2键盘/鼠标 | 当前只实现PS/2键盘；纯USB输入不适用 |
| 存储 | SATA / Intel AHCI | 内核无VBox IDE/SCSI/NVMe通用驱动；光驱由固件引导 |
| 网卡 | 首先关闭；随后NAT + Intel PRO/1000 MT Desktop / 82540EM | 不选默认AMD PCnet；82545EM也在驱动名单内 |
| 串口 | COM1：0x3f8，IRQ4，Raw File | 保存可检索日志；不是shell输入设备 |

Oracle关于串口模式的说明见 [Serial ports](https://docs.oracle.com/en/virtualization/virtualbox/6.0/user/serialports.html)。
命令块以本机7.2参数为基线；旧版可用GUI或按help调整别名/平台选项。
CLI选项参考 [VBoxManage](https://docs.oracle.com/en/virtualization/virtualbox/7.1/user/vboxmanage.html)，
不同版本GUI名称/CLI别名可能不同，以本机 `VBoxManage help modifyvm` 为准。

### 命令行建立新VM（可选，等价于GUI）

以下仅用于尚不存在的新VM名；已创建VM可直接在GUI配置，不要重复create/register。
日志目录/介质路径使用绝对路径。不要复用别的系统盘。

```bash
vm=CapMix-VBox
lab="$PWD/vbox-lab"
mkdir -p "$lab"
VBoxManage createvm --name "$vm" --platform-architecture x86 --ostype Other_64 --register
VBoxManage modifyvm "$vm" --memory 512 --cpus 4 --ioapic on --acpi on \
  --chipset piix3 --firmware efi64 --graphicscontroller vmsvga --vram 16 \
  --accelerate-3d off --keyboard ps2 --mouse ps2 --nic1 none \
  --boot1 dvd --boot2 disk --boot3 none --boot4 none
VBoxManage modifyvm "$vm" --uart1 0x3f8 4 \
  --uart-mode1 file "$lab/product-boot1.log"
VBoxManage storagectl "$vm" --name SATA --add sata --controller IntelAhci --portcount 2
VBoxManage storageattach "$vm" --storagectl SATA --port 1 --device 0 \
  --type dvddrive --medium "$PWD/capmix-product.iso"
VBoxManage startvm "$vm" --type gui
```

设置里若有EFI Secure Boot选项，确认关闭。当前内核无受信任签名链。
BIOS模式可作为另一次独立对照，但首轮推荐EFI获取FB；不能通过修改固件掩盖实际错误。

## 4. 无盘产品路径：屏幕、键盘、用户进程

看到 `init: ... root=ram`、`init ELF: pid=1 CPL=3`、
`shell ELF ready: CPL=3` 和 `capmix>`。点击虚拟屏捕获键盘，输入：

```text
help
echo vbox-keyboard-ok
ls /bin
run /bin/hello.elf
run /bin/hello-sdk.elf vbox first
run /bin/argtest.elf vbox argv
run /bin/ls.elf /bin
run /bin/cat.elf /example.txt
```

核对hello内容、argv[0]/参数、`cat bytes read`、`exit=0 reap=ok`。
`hello-sdk`使用宿主GCC与CapMix crt，证明不是仅能运行内嵌汇编hello。
屏幕与COM1日志均应有输出；默认serial stdin不接PS/2，向串口文件写字符不会执行命令。

无GOP时应有明确FB skip、串口仍可见；这覆盖降级，但不算“FB显示通过”。
无PS/2则不能完成交互；没有自动USB/串口输入回退。此时先检查VM设置。

`exit`结束shell并最终回到内核idle，不是关电；`poweroff`尝试ACPI S5，
不可用时safe-hlt，需从VBox关闭VM。正常产品测试用 `poweroff`，不要用保存状态替代冷启动。

## 5. SATA数据盘：用户文件与跨启动持久化

关闭VM，在SATA port0新建128MiB测试VDI（不存在的新路径）：

```bash
VBoxManage createmedium disk --filename "$lab/capmix-data.vdi" --size 128 --format VDI
VBoxManage storageattach "$vm" --storagectl SATA --port 0 --device 0 \
  --type hdd --medium "$lab/capmix-data.vdi"
VBoxManage startvm "$vm" --type gui
```

GUI新建VDI也可以，但必须接SATA/AHCI，不能只看文件扩展名。
在客户机内这是一块空白磁盘；M3策略只在保留卷窗口全空时创建COW，非空未知卷拒绝。
此时仍从ISO引导，磁盘只提供根数据，不是已安装的可引导系统盘。

第一次启动输入：

```text
run /bin/filetest.elf
cat /p1a/user.txt
run /bin/cat.elf /p1a/user.txt
poweroff
```

等待VM真正关闭，给COM1换新文件再启动同一VDI：

```bash
VBoxManage modifyvm "$vm" --uart-mode1 file "$lab/product-boot2.log"
VBoxManage startvm "$vm" --type gui
```

再运行filetest和cat。核对 `root=disk backend=ahci`、
`p1a previous boot payload verified`、内容一致、`exit=0 reap=ok`。
第二轮不应再次format。不要在两轮之间重建VDI或恢复旧快照，否则不是持久化测试。
容量仍是固定8MiB逻辑卷内约3.97MiB COW区，不是整块128MiB均可存文件。

## 6. NAT网络：DHCP、UDP、TCP、nc、HTTP

关机后将adapter1设为**NAT（不是NAT Network或桥接）**、82540EM、接通网线。
仅配置一块网卡，减少根因混淆：

```bash
VBoxManage modifyvm "$vm" --nic1 nat --nic-type1 82540EM --cable-connected1 on \
  --nat-localhostreachable1 on
```

本手册使用默认NAT网段：客户机通常10.0.2.15，宿主loopback通过10.0.2.2访问。
`nat-localhostreachable1`显式允许该访问；这不是guest端监听服务所需的端口转发。
说明见 [Oracle NAT说明](https://docs.oracle.com/en/virtualization/virtualbox/7.1/user/networkingdetails.html)
及 [NAT选项](https://docs.oracle.com/en/virtualization/virtualbox/7.1/user/vboxmanage.html)。
若改过NAT网段先恢复基线；若网关不是10.0.2.2，现有测试ELF的固定peer可能不适用。

在运行VirtualBox的**同一宿主**分别打开终端，仓库根启动：

```bash
python3 scripts/udp-echo.py          # UDP 127.0.0.1:9999
python3 scripts/tcp-echo.py          # TCP 127.0.0.1:9998
# 每个服务占一个终端，不是在同一前台依次执行。
```

另开终端准备HTTP：

```bash
mkdir -p /tmp/capmix-vbox-www
printf 'hello from VBox HTTP test\n' > /tmp/capmix-vbox-www/hello.txt
python3 -m http.server 8000 --bind 127.0.0.1 --directory /tmp/capmix-vbox-www
```

服务必须监听成功；若代码在另一台构建机，需把echo脚本复制到VBox宿主运行。
只绑定loopback即可，不必开放到整个局域网；宿主防火墙/安全软件仍可能影响可达性。

启动产品ISO，先核对 `e1000 ready: link=up`、`nic=enet0/up` 和DHCP ACK。
DHCP失败允许明确static fallback；这只证明回退，不能记为lease成功。

```text
run /bin/udpwire.elf
run /bin/tcpwire.elf
run -i /bin/nc.elf 10.0.2.2 9998
```

nc连接后输入 `vbox-echo`，应先看到本地键盘回显、随后网络echo以及
`nc tx bytes=N`、`nc rx bytes=N`；输入 `/quit` 回到shell，检查exit0/reap。

```text
run /bin/http-get.elf 10.0.2.2 8000 /hello.txt
run /bin/http-get.elf 10.0.2.2 8000 /not-found.txt
```

200应打印状态行、hello正文、`http-get exit=0`；404返回exit4并回收，不应误写为失败泄漏。
HTTP只接受数字IPv4，显示前1024B正文，接收上限1MiB；无TLS/DNS/重定向/chunked。
完整参数、超时和错误语义见 [EXAMPLES](EXAMPLES.md)。

### 后台收包与IRQ如何判定

产品RX worker自动启动，应用无需手动poll网卡。若日志为
`e1000 irq rx skipped ... fallback=poll-10ms`，这是当前平台路由边界内的预期回退；
后台worker仍能处理UDP/TCP。只有实际出现IRQ enabled、硬件计数增加和IRQ批次入队
断言，才能说该次IRQ通过。不要仅按“VBox也有IOAPIC”强开未知PCI GSI。

可选把adapter类型改为virtio再独立复测；只有暴露内核支持的legacy/transitional PCI
transport才能工作，modern-only会skip。VBox的virtio**网卡**不表示提供受支持的
virtio-blk**存储**控制器；存储基线仍用SATA/AHCI。

## 7. 并发、死循环kill和有界关机

```text
run /bin/concurrent.elf &
run /bin/hello-sdk.elf foreground
```

观察后台start、前台执行、各自reap；严格的overlap/同CPU阻塞现场断言在debug suite中。
shell最多跟踪8后台+1前台，进程总槽位32还包含PID1、shell和退出记录。

死循环测试：

```text
run /bin/spin.elf &
```

记下这次输出的PID，再输入 `kill 实际PID`；例如日志为pid=7才使用 `kill 7`。
应看到请求接受、最终exit137、reap，不以“接受请求”代替真正回收。
另起一个spin后直接 `poweroff`，核对5秒grace、超时kill、2秒收尾及ACPI关机。
仍有活跃内核现场时FORCE保留资源并跳过sync，此时不能断言未同步文件持久。
详见 [KILL_SHUTDOWN](KILL_SHUTDOWN.md)。

## 8. 安装盘：卸掉ISO后冷启动

本步骤与第5节的数据盘分开：先关闭VM，将原数据VDI从port0**仅移除附件**，
保留原文件备份；不要把现有数据盘交给blank-only安装器。

在构建机生成一个**不存在的新路径**RAW安装盘，再转换为VDI：

```bash
make install-disk DISK_IMAGE="$lab/capmix-system.img"
VBoxManage convertfromraw "$lab/capmix-system.img" "$lab/capmix-system.vdi" --format VDI
VBoxManage storageattach "$vm" --storagectl SATA --port 0 --device 0 \
  --type hdd --medium "$lab/capmix-system.vdi"
VBoxManage storageattach "$vm" --storagectl SATA --port 1 --device 0 \
  --type dvddrive --medium emptydrive
VBoxManage modifyvm "$vm" --boot1 disk --boot2 dvd \
  --uart-mode1 file "$lab/installed-boot1.log"
VBoxManage startvm "$vm" --type gui
```

若构建机没有VBoxManage，可先传RAW到VBox宿主再convert；`lab`路径按宿主实际位置调整。
这是一份完整磁盘镜像，不要把RAW作为光盘挂载，也不要把ISO改扩展名当硬盘。
安装器写Limine BIOS/UEFI和ESP，首次无ISO引导才初始化COW并provision系统文件。

首轮运行filetest、hello-sdk、http-get（有服务时），正常poweroff；第二次冷启动同一VDI，
应从盘进入 `/sbin/init.elf`、用户shell，filetest报告previous-boot内容。
确认光驱为空、`root=disk backend=ahci`、第二轮没有format；否则不算无ISO持久根通过。
若EFI进入固件菜单，可从ESP选择 `EFI/BOOT/BOOTX64.EFI`，检查EFI64/Secure Boot与附件。

VM运行后修改的是VDI，原RAW不会自动同步。外部SDK离线导入工具只接受已初始化
安装**RAW**，不能直接改VDI。若需测试自己编译的ELF，先正常关机，且不用snapshot链：

```bash
# 下面所有目标均使用不存在的新文件名。
VBoxManage clonemedium "$lab/capmix-system.vdi" "$lab/system-current.img" --format RAW
make build/cow-put build/example-http-get.elf
python3 scripts/copy-to-disk.py "$lab/system-current.img" build/example-http-get.elf /bin/my-http-get.elf
VBoxManage convertfromraw "$lab/system-current.img" "$lab/system-imported.vdi" --format VDI
```

再将新VDI接到port0代替旧VDI，保留旧盘，启动后 `run /bin/my-http-get.elf 10.0.2.2 8000 /hello.txt`。
不要用最初尚未初始化的RAW导入，也不要并发编辑活跃盘；不存在目标/拒绝覆盖的语义见
[外部ELF](EXTERNAL_ELF.md)、固定布局见 [INSTALL_DISK](INSTALL_DISK.md)。

## 9. debug全量：4 CPU与串口验收

先关机、卸下持久盘或换其备份；将selftest ISO接port1，启动顺序DVD优先。
第一轮关闭NIC，减少平台ICMP差异对核心测试的干扰：

```bash
VBoxManage storageattach "$vm" --storagectl SATA --port 0 --device 0 --type hdd --medium none
VBoxManage storageattach "$vm" --storagectl SATA --port 1 --device 0 \
  --type dvddrive --medium "$PWD/capmix-selftest.iso"
VBoxManage modifyvm "$vm" --boot1 dvd --boot2 disk --nic1 none \
  --uart-mode1 file "$lab/selftest-core.log"
VBoxManage startvm "$vm" --type gui
```

每轮使用新串口文件名，避免旧PASS混入当前验收。
自测结束内核idle不主动关机；等总结果出现后从管理器关闭，勿保存运行状态继续下一轮。
可用 `tail -f "$lab/selftest-core.log"` 看进度，不以固定30秒超时替代最终结果。

```bash
grep -iE 'PASS|PASSED|FAIL|buddy SMP|timer calib|preempt|overlap|phase[0-9]|numa|vmx|ept|trace|sign' \
  "$lab/selftest-core.log"
```

重点核对：

| 项目 | 日志/断言 |
| --- | --- |
| 总结果 | `selftest result: PASS (0 failures)`；发现FAIL时保留完整日志 |
| buddy | 4 CPU soak、总40000轮/160000次分配配对释放，duplicate/corrupt/retained=0 |
| timer | deadline状态、calib_hz、BSP/AP armed，逐CPU `timer calib ... PASS`，平均误差<5% |
| ELF | `elf user preempt verified`、参数校验、双ELF overlap、同CPU阻塞隔离、生命周期无泄漏 |
| phase3 | pressure/COW `PASSED` |
| VMO/驱动 | transfer/shared-write/lifecycle/rollback；睡眠→IRQ→唤醒→recv；crash isolation/kernel_alive |
| TRACE | injected/readback verified、per-CPU顺序/计数、ring3 granted/denied、覆写lost |
| 签名 | SHA-256 NIST向量、digest accept/reject；不是X.509/公钥签名 |
| KPTI/NUMA | 实际shadow/full切换、node0 fallback；SMEP/SMAP按实际值，KASLR unimplemented |
| VMX/EPT | 无VMX时明确guest launch skipped；有真实路径时 reason18/48、rollback=ok、buddy可用 |

然后再开启NAT，以普通selftest ISO测试NIC初始化/ARP/ICMP与DHCP。
VBox NAT的ICMP行为有平台限制，若M8/probe FAIL，记录原错误；不能把它改算
“无设备skip”或只截最后几行隐藏FAIL。产品TCP/HTTP通过也不能替代整套debug PASS。
参见 [Oracle网络限制](https://docs.oracle.com/en/virtualization/virtualbox/7.1/user/networkingdetails.html)。

### 网络/RTO专项

在宿主echo服务就绪后，另构建：

```bash
make iso LIMINE_CONFIG=boot/limine-tcp-rto-selftest.conf
cp capmix.iso capmix-network-selftest.iso
make iso
```

关机换挂network-selftest ISO、启用NAT、串口写 `selftest-network.log`。
该配置启用真实UDP/TCP echo与首次SYN/数据/FIN丢包；核对payload、retransmit、
loss recovery、用户TCP ELF、最终live回到基线以及总PASS。服务未启动就是测试条件不满足，
不可假装成功。9997端口应无人监听，供失败/回收负例使用。

### 可选nested VMX

先完成默认配置回归，再在关机状态尝试：

```bash
VBoxManage modifyvm "$vm" --nested-hw-virt on
```

宿主CPU/虚拟化后端不一定向客户机暴露VMX/EPT/INVEPT；设置开关不等于已提供能力。
日志中明确真实VMLAUNCH/VMCALL reason18、故意未映射GPA reason48、rollback=ok，
才算相应硬件路径运行；仅SVM或必要能力不足应保持skip。
默认可重新设off验证无VMX回退。VBox界面能开nested不代表已实现SVM guest或Linux客户机。

## 10. 负例、故障定位与记录模板

| 现象 | 先检查什么 |
| --- | --- |
| 无启动介质/EFI shell | ISO路径与附件、DVD优先、安装盘ESP、EFI64、Secure Boot |
| 黑屏但串口正常 | GOP/FB ready或skip、显示控制器、EFI、3D关闭；区分日志与图形输出 |
| 输入无效 | 先点击窗口捕获，PS/2键盘；Host键释放；不使用纯USB/串口输入 |
| 根仍RAM | 是否接SATA/AHCI、空盘/已识别COW、后端初始化日志；不要清空未知非空盘“修复” |
| 新程序不出现 | 旧磁盘文件不覆盖；旧ESP内核不会自动升级；ISO与安装盘内核版本可能不同 |
| DHCP fallback | 网线、NAT、DHCP报文；可测 `capmix.dhcp_drop=1` 强制超时，不把fallback记为ACK |
| TCP/HTTP失败 | 本机8000/9998/9999服务、NAT localhostreachable、10.0.2.2、错误码；非TLS/DNS客户端 |
| IRQ skipped | 当前平台路由限制，先验证fallback的收发；不能据此要求VM伪装i440FX |
| tick/soak失败 | 实际4 vCPU、宿主负载、calib/armed日志；平均频率与单次jitter分开记录 |
| VMX skipped | nested开关、实际CPUID/MSR能力；skip保留原因，不伪报真实vmexit |
| poweroff后窗口不关闭 | 查ACPI S5或safe-hlt；降级停机不是崩溃，也不是宿主关机 |

可独立准备Limine配置验证 `capmix.net=static`、默认/显式 `capmix.net=dhcp`、
`capmix.dhcp_drop=1`；不要编辑安装盘来覆盖未知数据。无NIC使用adapter none，
无GOP的降级只核对实际skip，本手册不提供随意破坏EFI映射的强制手段。

每次记录：VBox版本、宿主OS/CPU、VM CPU/RAM/固件/芯片组/网卡/存储、
ISO配置、是否nested、是否真冷启动、完整串口路径、FAIL/skip原因、应用exit/reap。
给出本次明确结论：核心suite PASS / 网络专项PASS / 产品路径通过 / 某硬件路径skip。
原有QEMU和历史VBox测试记录可作对照，不能替代你的这次结果。

返回 [文档导航](README.md)；体系与容量见 [内核总览](KERNEL_OVERVIEW.zh-CN.md)。

## 重定向续作的实测入口

产品可使用 `echo first > /note.txt`、`echo second >> /note.txt`、
`cat < /note.txt > /copy.txt`、`run /bin/cat.elf < /copy.txt > /elf-copy.txt`。
2026-10-15在独立VBox 7.2.20、EFI64/PIIX3、AHCI、4 CPU、512MiB上完成
写入→ACPI关机→同盘无ISO冷启动读回，内容断言PASS。e1000型号82540EM已枚举，
启动link down，网络未据此宣称通过。复现、权限与输出副作用见 [SHELL](SHELL.md)。
本机VBox与KVM/QEMU应串行运行，避免硬件虚拟化资源冲突。

### Linux L1 定制文件子集（2026-10-15）

仅对启用 `capmix.linux=1` 的 Linux 静态 ELF 生效；默认产品关闭，原生工具保留。
使用 `boot/limine-linux.conf` 构建/安装后，无 ISO 的 UEFI/AHCI 盘可运行：

```text
run /bin/busybox-l1.elf echo vbox-l1 > /vbox-l1
run /bin/busybox-l1.elf cat /vbox-l1
run /bin/busybox-l1.elf ls /
poweroff
```

再次冷启动后执行 cat，核对 `vbox-l1` 仍在。此配置为**定制 musl 静态 BusyBox 子集 +
有界 BSS 分配（64KiB）**，不支持任意 musl/BusyBox。详细 ABI、哈希和开关见
[LINUX_ABI_L1](LINUX_ABI_L1.md)。本轮 VBox7.2.20 SMP4 实测文件/重定向/冷启动读回；
新 VM 的 e1000 启动为 link-down，网络未记通过，也未运行 VBox 全量 debug。
