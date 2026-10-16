# Fixed static BusyBox hush subset

`busybox-sh.elf` is BusyBox 1.36.0 + musl 1.2.5, Linux-marked static x86-64.
SHA-256 is pinned in `busybox-sh.sha256` (checked from repository root).
This is a custom noninteractive subset, not arbitrary BusyBox/musl/glibc support.
It is separate from, and does not replace, the bounded-BSS L1 fixture.

Complete upstream source archives, hashes, origin and licenses are vendored at
`third_party/linux-l1/`. BusyBox is GPL-2.0-only; musl is MIT plus notices.
Distribute those archives and copyright/license notices with this executable.
Corresponding configuration, integer-printf and signal-error patches are at
`user/linux/`; exact build recipe is `scripts/build-busybox-sh.sh`.
The shell uses real musl malloc/brk/mmap, not `bss_alloc.c`.

```sh
scripts/build-busybox-sh.sh /tmp/fresh-busybox-sh
cmp /tmp/fresh-busybox-sh/busybox-sh.elf tests/fixtures/busybox-sh.elf
sha256sum -c tests/fixtures/busybox-sh.sha256
```

Two fresh directories reproduced identical stripped ELF bytes with GCC13.3.0 /
binutils2.42. The recipe audits the final executable for SIMD/FP loads/stores.
Signal delivery remains unavailable; the BusyBox patch handles sigaction failure
without reading an uninitialized old action. It does not implement signals.
See `docs/LINUX_BUSYBOX_SH.md` for coverage, errors, limits and environment matrix.
