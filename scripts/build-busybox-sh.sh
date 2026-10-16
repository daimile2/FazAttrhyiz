#!/bin/sh
# Offline build of the pinned custom subset. Requires gcc/binutils/make/patch/python3/bzip2.
set -eu
export SOURCE_DATE_EPOCH=0 TZ=UTC KCONFIG_NOTIMESTAMP=1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:-/tmp/capmix-busybox-sh-build}
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
cd "$work/busybox-1.36.0"
patch -p1 < "$root/user/linux/busybox-sh-signals.patch"
cp "$root/user/linux/busybox-sh.config" .config
make oldconfig </dev/null
# dump.o contains floating hexdump formatting, is built by libbb but not linked
# by this subset. Enable SSE just for that unused object; audit the final ELF.
make -j"${JOBS:-4}" CC="$work/tools/bin/musl-gcc" 'CFLAGS_dump.o=-msse2' EXTRA_LDFLAGS="-Wl,--build-id=none,-z,max-page-size=0x1000"
objdump -d busybox_unstripped > "$work/disassembly.txt"
if grep -Eq '%[xyz]mm|\<fld[a-z]*\>|\<fst[a-z]*\>' "$work/disassembly.txt"; then
    echo 'Unexpected SIMD/FP instruction in final subset' >&2; exit 1
fi
cp busybox_unstripped "$work/busybox-sh.elf"
strip --strip-all "$work/busybox-sh.elf"
# Explicit Linux evidence; musl's default SYSV OSABI alone does not select persona.
python3 - "$work/busybox-sh.elf" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); b=bytearray(p.read_bytes()); assert b[:5]==b'\x7fELF\x02'; b[7]=3; p.write_bytes(b)
PY
sha256sum "$work/busybox-sh.elf"
echo "Built custom musl static BusyBox hush subset + real malloc/brk/mmap (not general BusyBox); compare before replacing fixture."
