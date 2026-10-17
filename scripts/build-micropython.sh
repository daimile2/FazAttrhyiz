#!/bin/sh
# Offline, pinned MicroPython Linux-user scalar subset; no changes to other fixtures.
set -eu
export SOURCE_DATE_EPOCH=0 TZ=UTC LC_ALL=C PYTHONHASHSEED=0
# Never inherit the parent CapMix checkout tag/hash when building inside it.
export MICROPY_GIT_TAG=v1.24.1 MICROPY_GIT_HASH="<no hash>"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:?usage: scripts/build-micropython.sh /path/to/fresh-directory}
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
if [ -e "$work/musl-1.2.5" ] || [ -e "$work/micropython-1.24.1" ]; then
    echo 'Use a fresh build directory' >&2; exit 1
fi
(cd "$root" && sha256sum -c third_party/linux-l1/SHA256SUMS && sha256sum -c third_party/micropython/SHA256SUMS)
tar -xf "$root/third_party/linux-l1/musl-1.2.5.tar.gz" -C "$work"
tar -xf "$root/third_party/micropython/micropython-1.24.1.tar.gz" -C "$work"
cd "$work/musl-1.2.5"
patch -p1 < "$root/user/linux/musl-integer-printf.patch"
./configure --prefix="$work/tools" --disable-shared CFLAGS='-Os -fno-stack-protector -fno-tree-vectorize -mno-red-zone'
make -j"${JOBS:-4}"
python3 - "$root/user/linux/musl-scalar-units.txt" <<'PY'
from pathlib import Path
import sys
for name in Path(sys.argv[1]).read_text().splitlines():
    Path(name).unlink(missing_ok=True)
PY
make -j"${JOBS:-4}" $(cat "$root/user/linux/musl-scalar-units.txt") CFLAGS='-Os -fno-stack-protector -mno-red-zone -mgeneral-regs-only'
make lib/libc.a
make install
cd "$work/micropython-1.24.1"
mkdir -p ports/capmix
cp "$root"/user/linux/micropython/* ports/capmix/
make -C ports/capmix -j"${JOBS:-4}" CC="$work/tools/bin/musl-gcc"
cp ports/capmix/build/micropython.elf "$work/micropython.elf"
objdump -d "$work/micropython.elf" > "$work/disassembly.txt"
if grep -Eq '%([xyz]mm|mm[0-7]|st\()|[[:space:]](fld[a-z]*|fst[a-z]*|fadd[a-z]*|fsub[a-z]*|fmul[a-z]*|fdiv[a-z]*|fcom[a-z]*|fsqrt)[[:space:]]' "$work/disassembly.txt"; then
    echo 'Unexpected SIMD/FP instruction in final static interpreter' >&2; exit 1
fi
strip --strip-all "$work/micropython.elf"
python3 "$root/scripts/mark-linux.py" "$work/micropython.elf"
sha256sum "$work/micropython.elf"
echo 'Built custom MicroPython1.24.1 static scalar port: 256KiB GC, no float/threads/JIT/external-modules/REPL. Not general Python/Linux.'
