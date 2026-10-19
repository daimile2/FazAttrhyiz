# 屏幕控制台大小写与可读性

2026-10-19。本轮修复 Limine/GOP 帧缓冲字体：旧代码收到 `a` 却取 `A` 的字形，导致日志、命令、路径看起来全部大写。现在为 `a`–`z` 提供独立字形，并利用字符格内的第八行显示 `g/j/p/q/y` 的下伸部分。日志按原文显示，保留 `[INF]`、`[SHELL ...]` 等标签；没有强制把所有文本转成小写。

## 使用者看到的变化

1. 启动产品镜像，屏上提示符为 `capmix>`，日志中的英文小写也实际显示为小写。
2. 直接用 PS/2 键盘输入 `help`、`cat /case.txt`，默认产生小写并原样回显。键盘原先已经产生小写，支持左右 Shift：按住时输入大写，松开恢复小写；Caps Lock 每次按下切换大写锁定。Caps Lock 开启时 Shift+字母输入小写，数字和符号不受 Caps Lock 影响。
3. 路径始终按原始字节区分大小写，`/bin`、`/Bin`、`/BIN` 是不同名称。没有更改 COWFS 的比较规则，没有隐式转小写，也没有改变盘上数据。
4. 串口内容保持原样；无 framebuffer 时仍使用既有 skip/串口回退。

字体仍是紧凑 ASCII 字体，不宣称 Unicode、中文字体、文字编码转换或大小写归一化。原有键盘 Shift/Caps 兼容行为保留；现已补齐 Caps Lock 长按去重，避免键盘重复 make 扫描码反复切换；不涉及屏幕切换界面。大小写敏感不仅是视觉区别，文件名实际也不同。

## 可复现验收

先创建一次性安装盘，然后脚本通过虚拟 PS/2 键盘操作产品 shell；两次冷启动均不挂 ISO。目录必须新建，脚本不会覆盖现有实验盘。

```sh
scripts/demo-user-journey.sh prepare /tmp/capmix-case-qemu
python3 tests/check_user_journey.py --backend qemu --scenario console-case --lab /tmp/capmix-case-qemu

# 与 QEMU 顺序运行，不要同时占用硬件虚拟化
scripts/demo-user-journey.sh prepare /tmp/capmix-case-vbox
python3 tests/check_user_journey.py --backend vbox --scenario console-case --lab /tmp/capmix-case-vbox
```

脚本写入 `/case.txt` 与 `/Case.txt`，分别读回 `lower-file`、`upper-file`；不存在的 `/CASE.txt` 返回 `-12`。冷启动再校验，两份文件仍独立。它还输入两套字母及路径样例，并保存 `case-first.png`、`case-second.png` 实际 framebuffer 截图。大写样例用已有 Shift 验证原样显示，默认输入仍为小写。

hosted 自测检查 26 对字形不同、下伸像素、退格清除、滚屏和 framebuffer 边界，日志关键字：

- `fb ASCII case: a-z distinct from A-Z; path bytes preserved`
- `fb hosted selftest: mixed-case glyph/descender/backspace/scroll/pitch bounds verified`
- `ps2 selftest: default lowercase a-z/path`

环境与证据见下表；产品定向验收与全量 selftest 分开记录。

| 日期 | 环境 | 范围 | 结果/证据 |
|---|---|---|---|
| 2026-10-19 | hosted | 全量；字形/退格/滚屏/边界/键盘 | [PASS (0 failures)](../tests/evidence/logs/console-case-hosted.log) |
| 2026-10-19 | QEMU/KVM，4 CPU，AHCI，e1000，Linux 默认关，无 ISO | 产品 PS/2 输入、大小写路径、两次冷启动 | [PASS](../tests/evidence/logs/console-case-qemu-check.log)；[首次串口](../tests/evidence/logs/console-case-qemu-case-first.log)、[冷启动](../tests/evidence/logs/console-case-qemu-case-second.log)、[屏幕](../tests/evidence/screenshots/console-case-qemu-case-first.png) |
| 2026-10-19 | VirtualBox，UEFI，4 CPU，PIIX3，VMSVGA，AHCI，e1000，Linux 默认关，无 ISO | 同上；实际截图人工核对 | [PASS](../tests/evidence/logs/console-case-vbox-check.log)；[首次串口](../tests/evidence/logs/console-case-vbox-case-first.log)、[冷启动](../tests/evidence/logs/console-case-vbox-case-second.log)、[屏幕](../tests/evidence/screenshots/console-case-vbox-case-first.png) |
| 2026-10-19 | QEMU/KVM，4 CPU，无网卡，Linux/解释器开启 | 全量机制回归；网络外部服务测试按既有语义 skip | [PASS (0 failures)](../tests/evidence/logs/console-case-debug.log) |

[编译日志](../tests/evidence/logs/console-case-kernel.log)无 warning/error；[debug ISO 构建](../tests/evidence/logs/console-case-debug-iso.log)成功。[截图 SHA-256](../tests/evidence/logs/console-case-screenshots.log)用于核对未修改的屏幕捕获。

第一次全量运行误用了要求宿主 echo peer 的配置，未启动相应服务：[原始失败日志](../tests/evidence/logs/console-case-debug-missing-peers.log)保留，7 failures 不算通过。随后移除外部 echo 测试选项并明确 `-nic none`，全量为 0 failures。没有删断言或修改网络实现。本轮 VBox 验证产品控制台，不宣称 VBox 全量 debug 或网络压力回归。

默认产品 ISO 已恢复并重新构建：[构建](../tests/evidence/logs/console-case-product-iso.log)、[启动到用户 shell](../tests/evidence/logs/console-case-product.log)。该启动检查到提示符后由宿主 timeout 结束，不等同于主动关机测试；两套安装盘测试另有实际 ACPI 关机记录。

## Shift / Caps Lock 输入

左右 Shift 可独立按住；两者都按住时，释放一个仍保持 Shift。Caps Lock 只在一次新的按下时切换，长按重复不会反复开关。默认小写，Shift 与 Caps Lock 对字母采用异或规则。暂不控制实体键盘 Caps Lock 指示灯。

机制自测日志：`ps2 modifiers: held left/right Shift + CapsLock toggle/repeat + Shift XOR Caps verified`。产品脚本也注入 Caps 开启→`abc`→Shift+A→Caps 关闭→`def`，必须读到 `ABCadef`，验证实际 PS/2 通路。

2026-10-19 Shift/Caps 增量验收：[hosted 全量 PASS](../tests/evidence/logs/keyboard-modifiers-hosted.log)、[QEMU smp4 全量 PASS](../tests/evidence/logs/keyboard-modifiers-debug.log)；[QEMU 产品按键/冷启动 PASS](../tests/evidence/logs/keyboard-modifiers-qemu-check.log)、[VBox UEFI smp4 产品按键/冷启动 PASS](../tests/evidence/logs/keyboard-modifiers-vbox-check.log)。实际 `ABCadef` 输出见 [QEMU](../tests/evidence/logs/keyboard-modifiers-qemu-case-first.log)、[VBox](../tests/evidence/logs/keyboard-modifiers-vbox-case-first.log)。[kernel 构建](../tests/evidence/logs/keyboard-modifiers-kernel.log)无 warning/error，[ISO 构建](../tests/evidence/logs/keyboard-modifiers-product-iso.log)成功且恢复默认产品配置。
