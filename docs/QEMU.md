# 在自己电脑上用 QEMU 跑 CapMix

两条路径，不要混：

| 路径 | 测什么 | 要不要 QEMU |
|---|---|---|
| `make test` | 阶段 0–8 全部 selftest | 不要 |
| `make iso && ./scripts/qemu.sh` | 裸机启动 + 串口打印同一套 selftest | 要 |

**先把 `make test` 跑绿。** QEMU 只证明 Limine 能把内核捞起来、串口能说话。子系统逻辑在宿主机模式里更完整、更好 debug。

## 0. 依赖（Debian / Ubuntu / Pop）

```bash
sudo apt update
sudo apt install -y build-essential qemu-system-x86 qemu-system-gui \
    xorriso mtools wget xz-utils
```

Arch：

```bash
sudo pacman -S base-devel qemu-system-x86 xorriso mtools wget
```

macOS（Homebrew，用用户态加速即可）：

```bash
brew install qemu xorriso wget
```

## 1. 拿源码并先跑宿主机自测

```bash
cd capmix
make test
```

看到 `selftest result: PASS` 和 `phase-0..8 complete` 再往下。

## 2. 拉 Limine，打 ISO

脚本会把官方 Limine 二进制下到 `third_party/limine/`（约几 MB）：

```bash
chmod +x scripts/qemu.sh scripts/fetch-limine.sh
./scripts/fetch-limine.sh
make iso
```

产物：`capmix.iso`。

手动等价步骤（脚本挂了就照这个敲）：

```bash
# 假设 third_party/limine 里已有 limine-bios-cd.bin / limine-uefi-cd.bin / limine-bios.sys
mkdir -p iso/boot/limine iso/EFI/BOOT
cp capmix.elf           iso/boot/capmix.elf
cp boot/limine.conf     iso/boot/limine/limine.conf
cp third_party/limine/limine-bios.sys      iso/boot/limine/
cp third_party/limine/limine-bios-cd.bin   iso/boot/limine/
cp third_party/limine/limine-uefi-cd.bin   iso/EFI/BOOT/
cp third_party/limine/BOOTX64.EFI          iso/EFI/BOOT/   # 若发行包提供

xorriso -as mkisofs -R -r -J \
    -b boot/limine/limine-bios-cd.bin -no-emul-boot \
    -boot-load-size 4 -boot-info-table \
    --efi-boot EFI/BOOT/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label \
    iso -o capmix.iso

./third_party/limine/limine bios-install capmix.iso   # 若有 limine 工具
```

Limine 8+ 的目录名、文件名偶有变化，以 [Limine 发行页](https://github.com/limine-bootloader/limine/releases) 为准。配置里内核路径是 `boot():/boot/capmix.elf`。

## 3. 启动 QEMU（串口在终端里）

BIOS：

```bash
qemu-system-x86_64 \
    -m 256M -smp 2 \
    -cdrom capmix.iso \
    -serial stdio \
    -display none \
    -no-reboot
```

UEFI（需要 OVMF 固件）：

```bash
# Debian 固件一般在：
#   /usr/share/OVMF/OVMF_CODE_4M.fd
#   或 /usr/share/ovmf/OVMF.fd

qemu-system-x86_64 \
    -m 256M -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -cdrom capmix.iso \
    -serial stdio \
    -display none \
    -no-reboot
```

成功时终端应出现：

```
CapMix 0.8.0-phase8 — capability hybrid kernel
freestanding: arena … pages
… selftest …
selftest result: PASS
freestanding selftest rc=0 — hlt
```

`-serial stdio` 把 COM1 接到你当前终端，这是阶段 0 的验证项（「能打印」）。

带窗口看 Limine 菜单（timeout 已设成 0，会直接进内核）：

```bash
qemu-system-x86_64 -m 256M -cdrom capmix.iso -serial stdio
```

## 4. 常见失败

| 现象 | 原因 | 处理 |
|---|---|---|
| QEMU 黑屏、终端没字 | 没加 `-serial stdio` | 内核日志只走 COM1 |
| `no bootable device` | ISO 里缺 Limine stage | 重跑 `fetch-limine.sh`，核对 `iso/boot/limine/` |
| `kernel_path` 找不到 | elf 没拷到 `iso/boot/capmix.elf` | `make iso` |
| 三重错误 / 立刻 reboot | 裸机缺 memcpy 或栈 | 确认 `make kernel` 用了 `-ffreestanding -nostdlib` 和 `lib/kstring.c` |
| `make kernel` 链接失败 | 主机 gcc 开了 PIE/stack-protector | Makefile 已关；再加 `-fno-pie` |
| selftest 在 QEMU 里挂在 buddy | 竞技场没对齐 | `g_arena` 必须 `aligned(4096)` |

加速（可选）：

```bash
qemu-system-x86_64 -enable-kvm -cpu host -m 256M -cdrom capmix.iso -serial stdio -display none
```

只在 Linux 且 CPU 支持 VT-x/AMD-V 时加 `-enable-kvm`。

## 5. 现在 QEMU 里实际跑到哪

内核是 **Limine 协议的 higher-half ELF**，入口 `kmain`：

1. 用 2MiB BSS 竞技场当物理页（还没解析 Limine mmap）
2. 走和 `make test` 同一套 `capmix_init` + `capmix_selftest`
3. 串口打完后 `hlt`

还不是一个能装磁盘、进 shell 的操作系统。那是阶段 2 用户态在真分页 + `sysretq` 之后的事。QEMU 的意义是：确认 boot 链和串口，而不是替代宿主机自测。
