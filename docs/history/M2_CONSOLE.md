# M2 Guest console

> 历史阶段记录：保留当时实现与验收边界；后续功能可能已扩展。当前状态见
> [内核总览](../KERNEL_OVERVIEW.zh-CN.md)，操作测试见 [VirtualBox手册](../VIRTUALBOX_TESTING.zh-CN.md)。

Boot logs go to COM1 and, when Limine provides a supported linear framebuffer,
to the virtual screen. This uses Limine's framebuffer interface (UEFI GOP or
BIOS framebuffer), not a GPU driver. The descriptor is copied while Limine's
page tables still exist; the console attaches after CapMix's own page tables
are live, using the existing uncached MMIO mapper. The earliest pre-paging logs
remain serial-only. No VMX/EPT, TRACE, NUMA or KPTI mapping core was changed.

Supported pixels: RGB 24/32 bpp with distinct 8-bit component masks; dimensions
and pitch are bounded before mapping. Missing/unsupported framebuffer or mapping
failure prints `fb console skipped` and preserves serial operation. Original
5x7 glyphs cover printable ASCII, with lowercase rendered using uppercase
shapes. The text grid is at most 100x40; large displays use 2x glyph scaling.
Newline, carriage return, tab, backspace and scrolling are implemented. There is
no Unicode/ANSI escape parser or graphical acceleration.

The PS/2 controller is initialized with bounded waits and translated set-1
scancodes. IRQ1 consumes controller bytes into a bounded SPSC queue, discarding
AUX/parity/timeout bytes and counting overflow. The existing virtual IRQ injection
and LAPIC EOI remain. There is no rendering/logging in the keyboard top half.
The BSP post-selftest idle loop decodes/echoes characters, with Shift, Caps Lock,
Enter, Tab and Backspace. Extended keys are ignored; no line discipline or user
input API is provided. Keyboard echo becomes available after selftests finish;
input during tests is queued up to 127 bytes. A full queue drops new bytes.
USB-only keyboards are outside scope; unavailable i8042/keyboard logs a skip.

## Build and run

```
make kernel
make iso
make -B test
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display gtk -no-reboot
```

Use `-display none` for serial-only host presentation; a virtual VGA can still
supply a framebuffer. To test missing framebuffer, add `-vga none`. To test a
remote visible display, use `-display vnc=127.0.0.1:7` and connect a VNC client
locally to port 5907. The existing scripts/qemu.sh also accepts display overrides
through QEMU_EXTRA. The kernel remains in an interrupt-enabled idle loop after
PASS, so timeout exit 124 is expected.

QEMU UEFI example (OVMF package paths may differ):

```
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/capmix-vars.fd
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/capmix-vars.fd \
  -cdrom capmix.iso -serial stdio -display gtk -no-reboot
```

Makefile now adds the UEFI El Torito entry alongside the existing BIOS entry.
The ISO includes Limine's EFI boot image and BOOTX64.EFI. Do not enable Secure
Boot: this development boot chain has no firmware-trusted signature.

## VMware UEFI notes

Power off the VM, choose UEFI/EFI firmware in its boot options, disable Secure
Boot, attach capmix.iso as a connected CD/DVD at power-on, then boot the virtual
optical drive. In Workstation the firmware controls are under VM Settings,
Options, Advanced; vSphere uses VM Options, Boot Options. VMware documents EFI
and Secure Boot configuration in [Broadcom's UEFI guide](https://community.broadcom.com/vmware-cloud-foundation/viewdocument/using-efiuefi-firmware-in-a-vmware?CommunityKey=e2de134c-2676-4a0a-9418-0dfb3a8e611e&tab=librarydocuments)
and [Secure Boot settings](https://knowledge.broadcom.com/external/article/377377).

Keep the standard virtual display available so the firmware can supply GOP;
GPU acceleration is not needed. Capture keyboard focus in the VM console. The
kernel needs the virtual i8042/PS/2 keyboard: if the particular VMware setup
exposes only USB input, this implementation cannot echo it. A virtual COM1
serial file remains useful for complete logs and skip diagnostics. VMware itself
was not available for this implementation's tests; no VMware PASS is claimed.

## Observable checks

- `fb console ready` and `fb selftest: active log output`, or an explicit skip.
- `fb hosted selftest: glyph/backspace/scroll/pitch bounds verified`.
- `ps2: ready set1 IRQ1 echo` and `ps2 selftest: ... verified`.
- After PASS: `guest console ready`; first character logs `ps2 echo: IRQ -> queued scancode -> character`.
- QEMU monitor `sendkey h`, `sendkey e`, `sendkey l`, `sendkey l`, `sendkey o`,
  `sendkey ret`, then `screendump /tmp/console.ppm` verifies real keyboard echo
  and screen pixels. Hosted does not pretend to exercise hardware.
