# Linux L1 source bundle

This directory contains full, unmodified corresponding upstream source archives:

- BusyBox 1.36.0: https://busybox.net/downloads/busybox-1.36.0.tar.bz2
  GPL-2.0-only; see `LICENSE`, `COPYING` and individual notices inside archive.
- musl 1.2.5: https://musl.libc.org/releases/musl-1.2.5.tar.gz
  Download used Debian's byte-identical upstream source mirror:
  https://deb.debian.org/debian/pool/main/m/musl/musl_1.2.5.orig.tar.gz
  MIT plus individual notices; see `COPYRIGHT` inside archive.

`SHA256SUMS` is checked from the repository root. The distributed executable is
**a custom musl static BusyBox subset + bounded BSS allocation**, not stock
BusyBox or arbitrary musl compatibility. It enables only echo/cat/ls.

Corresponding local configuration, allocator, scalar object selection and musl
integer-only printf patch are in `user/linux/`; the offline build procedure is
`scripts/build-busybox-l1.sh`. Nothing is downloaded by normal kernel builds.
The original source licenses and copyright notices remain inside the archives;
copy them along with this directory when distributing the fixture. See
`docs/LINUX_ABI_L1.md` for limitations, hashes and reproduction commands.
