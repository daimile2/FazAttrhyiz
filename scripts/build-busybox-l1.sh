#!/bin/sh
# Offline build of the pinned custom subset. Requires gcc/binutils/make/patch/python3/bzip2.
set -eu
export SOURCE_DATE_EPOCH=0 TZ=UTC KCONFIG_NOTIMESTAMP=1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:-/tmp/capmix-busybox-l1-build}
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
# A fresh directory avoids silently inheriting another libc/configuration.
if [ -e "$work/musl-1.2.5" ] || [ -e "$work/busybox-1.36.0" ]; then
    echo "Use a fresh build directory: $work" >&2; exit 1
fi
(cd "$root" && sha256sum -c third_party/linux-l1/SHA256SUMS)
tar -xf "$root/third_party/linux-l1/musl-1.2.5.tar.gz" -C "$work"
tar -xf "$root/third_party/linux-l1/busybox-1.36.0.tar.bz2" -C "$work"
cd "$work/musl-1.2.5"
patch -p1 < "$root/user/linux/musl-integer-printf.patch"
./configure --prefix="$work/tools" --disable-shared CFLAGS='-Os -fno-stack-protector -fno-tree-vectorize -mno-red-zone'
make -j"${JOBS:-4}"
# Integer-only user context: rebuild the selected linked C units without SIMD.
# FP scan/math code is outside this subset; it is not used by the three applets.
python3 - "$root/user/linux/musl-scalar-units.txt" <<'PY'
from pathlib import Path
import sys
for name in Path(sys.argv[1]).read_text().splitlines():
    Path(name).unlink(missing_ok=True)
PY
make -j"${JOBS:-4}" $(cat "$root/user/linux/musl-scalar-units.txt") CFLAGS='-Os -fno-stack-protector -mno-red-zone -mgeneral-regs-only'
make lib/libc.a
make install
"$work/tools/bin/musl-gcc" -Os -fno-builtin -fno-stack-protector -mno-red-zone -mgeneral-regs-only -c "$root/user/linux/bss_alloc.c" -o "$work/bss_alloc.o"
ar rcs "$work/libcapmix_bss.a" "$work/bss_alloc.o"
cd "$work/busybox-1.36.0"
cp "$root/user/linux/busybox.config" .config
make oldconfig </dev/null
make -j"${JOBS:-4}" CC="$work/tools/bin/musl-gcc" CFLAGS_EXTRA='-Os -fno-stack-protector -mno-red-zone -mgeneral-regs-only' EXTRA_LDFLAGS="-Wl,--build-id=none,-z,max-page-size=0x1000,--whole-archive,$work/libcapmix_bss.a,--no-whole-archive"
cp busybox_unstripped "$work/busybox-l1.elf"
strip --strip-all "$work/busybox-l1.elf"
# Explicit Linux evidence; musl's default SYSV OSABI alone does not select persona.
python3 - "$work/busybox-l1.elf" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); b=bytearray(p.read_bytes()); assert b[:5]==b'\x7fELF\x02'; b[7]=3; p.write_bytes(b)
PY
sha256sum "$work/busybox-l1.elf"
echo "Built custom musl static BusyBox subset + bounded BSS allocation; compare before replacing fixture."
