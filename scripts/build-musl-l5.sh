#!/bin/sh
# No libc/allocator patches: pinned musl 1.2.5 static malloc+mmap probe.
set -eu
export SOURCE_DATE_EPOCH=0 TZ=UTC
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:-/tmp/capmix-musl-l5-build}
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
test ! -e "$work/musl-1.2.5" || { echo 'Use a fresh build directory' >&2; exit 1; }
(cd "$root" && sha256sum -c third_party/linux-l1/SHA256SUMS)
tar -xf "$root/third_party/linux-l1/musl-1.2.5.tar.gz" -C "$work"
cd "$work/musl-1.2.5"
./configure --prefix="$work/tools" --disable-shared CFLAGS='-Os -fno-stack-protector -fno-tree-vectorize -mno-red-zone'
make -j"${JOBS:-4}"
make install
cp "$root/user/linux/mmap_probe.c" "$work/l5-musl-probe.c"
"$work/tools/bin/musl-gcc" -static -Os -fno-stack-protector -mno-red-zone -fno-tree-vectorize -Wl,--build-id=none,-z,max-page-size=0x1000 -o "$work/musl-mmap.elf" "$work/l5-musl-probe.c"
python3 "$root/scripts/mark-linux.py" "$work/musl-mmap.elf"
sha256sum "$work/musl-mmap.elf"
echo 'Built fixed musl static probe, no libc patch and no BSS allocator override; not general musl/glibc support.'
