# M5: PID1 and the product shell

`make iso` now boots the product path by default (`capmix.mode=product`). It
initializes the platform, mounts the existing COW root, and starts PID1 without
running the phase selftests. With a supported virtio-blk device the root stays on
that device; without one it uses the existing RAM backend. An unsafe/unreadable
root is refused rather than replaced. Product boot never switches to the M3
scratch root.

PID1 and its shell are native C-entry kernel tasks in this first version. The
`run` command launches a genuine CPL=3 static ET_EXEC process through the existing
ELF loader, with timer preemption. This is not a separately loaded user-mode shell.
There is one synchronous ELF child at a time, pinned to the BSP; completed child
images, task records and process slots are reclaimed before the next command.
AP scheduling stays disabled in the product runner. There is no job control or
Ctrl-C termination; an ELF that does not exit keeps `run` waiting.

## Commands and input

PS/2 keyboard input is echoed to the existing framebuffer console and serial:

```
capmix> help
capmix> echo hello
capmix> ls /bin
capmix> cat /absolute/path
capmix> run /bin/hello.elf
capmix> exit
```

`ls` defaults to `/`. Paths must be absolute, with no whitespace. Backspace edits
the line. Lines hold at most 255 characters; an overflowing line is rejected on
Enter, never executed as a truncated command. `cat` prints at most 4096 bytes and
substitutes `.` for nontext bytes. Quotes, pipes, redirects and history are absent.
`exit` syncs the root and returns to the kernel's console idle loop, without
powering off. Serial output mirrors the framebuffer, but serial input is not a
shell input source. Without a framebuffer, serial output still works; use a PS/2
keyboard or QEMU monitor `sendkey` for input. No USB input stack is added.

When `/bin/hello.elf` is missing, init installs the existing embedded test ELF via
VFS and fsync. An existing executable is preserved. The build generates it from
`tests/hello_elf.S` and `tests/hello_elf.ld`, then embeds `build/hello.elf` through
`scripts/embed-elf.py`. A virtio-backed root retains it across reboots. It uses the
CapMix syscall ABI, not Linux's ABI.

## Boot and regression testing

Example (create a dedicated blank disk once; do not truncate an existing disk):

```bash
truncate -s 32M /tmp/capmix-data.img
make kernel
make iso
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso \
  -drive file=/tmp/capmix-data.img,if=virtio,format=raw \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial stdio -display gtk -no-reboot
```

For the full debug pipeline, build the alternate boot configuration:

```bash
make iso LIMINE_CONFIG=boot/limine-selftest.conf
# Run the usual headless single/dual-node QEMU regression commands.
# Require: selftest result: PASS (0 failures)
make test
# Restore the default product ISO afterward:
make iso
```

Alternatively set `capmix.mode=selftest` in a Limine entry's cmdline. Tokens must
match exactly; the last recognized product/selftest token wins. No recognized
token means product. Hosted always runs selftests, regardless of cmdline.

Expected product logs: `boot mode: product`, `init: pid=1`, `shell ready`,
`capmix>`, `exec ring3`, `hello from elf`, and
`shell run done: ... exit=0 ... reclaim=0`. Debug additionally prints
`m5 shell selftest: ... verified` and the full selftest result. Product mode does
not claim a full selftest PASS because it deliberately does not run that suite.

VMware UEFI: retain the M2 Limine UEFI/GOP setup and firmware keyboard support;
use a keyboard exposing the supported PS/2 interface. M5 adds no VMware-specific
storage/network support: an unsupported device uses the existing RAM root.

M6 adds `poweroff`/`shutdown`, the product status banner and the explicit
[VMware UEFI setup and ACPI limits](M6_PRODUCT_BOOT.md).
