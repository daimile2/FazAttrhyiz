#!/bin/sh
# Original bounded rtld + golden ET_EXEC/DSO. GCC/binutils only, no downloads.
set -eu
export SOURCE_DATE_EPOCH=0 TZ=UTC
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:-/tmp/capmix-ldso-build}
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
cd "$root"
cc=${CC:-gcc}
flags='-std=c11 -Wall -Wextra -Wshadow -Os -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -mgeneral-regs-only -fPIC -fno-unwind-tables -fno-asynchronous-unwind-tables'
$cc $flags -c user/linux/ldso/gold.c -o "$work/gold.o"
ld -shared --hash-style=sysv --build-id=none -z max-page-size=0x1000 -z now -soname libcmgold.so.1 -o "$work/libcmgold.so.1" "$work/gold.o"
$cc $flags -c user/linux/ldso/main.c -o "$work/main.o"
$cc -c user/linux/ldso/main_start.S -o "$work/main_start.o"
ld --hash-style=sysv --build-id=none -z max-page-size=0x1000 -z now --dynamic-linker /lib/ld-capmix.so.1 -e _start -o "$work/linux-dynamic.elf" "$work/main_start.o" "$work/main.o" -L"$work" -l:libcmgold.so.1
# IE modules deliberately avoid __tls_get_addr / DTV / TLSDESC.
$cc $flags -ftls-model=initial-exec -c user/linux/ldso/tls_lib.c -o "$work/tls_a.o"
$cc $flags -ftls-model=initial-exec -DTLS_SECOND -c user/linux/ldso/tls_lib.c -o "$work/tls_b.o"
for module in a b; do
 ld -shared --hash-style=sysv --build-id=none -z max-page-size=0x1000 -z now -soname libcm_tls_$module.so.1 -o "$work/libcm_tls_$module.so.1" "$work/tls_$module.o"
done
$cc $flags -c user/linux/ldso/tls_main.c -o "$work/tls_main.o"
ld --hash-style=sysv --build-id=none -z max-page-size=0x1000 -z now --dynamic-linker /lib/ld-capmix.so.1 -e _start -o "$work/linux-tls.elf" "$work/main_start.o" "$work/tls_main.o" -L"$work" -l:libcm_tls_a.so.1 -l:libcm_tls_b.so.1
$cc $flags -fvisibility=hidden -c user/linux/ldso/ldso.c -o "$work/ldso.o"
$cc -c user/linux/ldso/start.S -o "$work/start.o"
ld -shared -Bsymbolic --hash-style=sysv --build-id=none -z max-page-size=0x1000 -z now -e _start -o "$work/ld-capmix.so.1" "$work/start.o" "$work/ldso.o"
python3 - "$work" <<'PY'
from pathlib import Path
import struct,sys
for name in ('linux-dynamic.elf','ld-capmix.so.1','libcmgold.so.1','linux-tls.elf','libcm_tls_a.so.1','libcm_tls_b.so.1'):
 p=Path(sys.argv[1])/name;b=bytearray(p.read_bytes());assert b[:7]==b'\x7fELF\x02\x01\x01';assert struct.unpack_from('<H',b,18)[0]==62;b[7]=3;p.write_bytes(b)
PY
for name in linux-dynamic.elf ld-capmix.so.1 libcmgold.so.1 linux-tls.elf libcm_tls_a.so.1 libcm_tls_b.so.1; do
 strip --strip-all "$work/$name"
 sha256sum "$work/$name"
done
readelf -d -r "$work/linux-dynamic.elf" "$work/libcmgold.so.1" "$work/ld-capmix.so.1" > "$work/dynamic-report.txt"

python3 scripts/check-ldso-fixtures.py "$work"

python3 scripts/check-tls-fixtures.py "$work"
