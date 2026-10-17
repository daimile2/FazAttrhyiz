#!/usr/bin/env python3
"""Verify the fixed scalar Linux-user ELF, host semantics and file reader bounds."""
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
elf = root / 'tests/fixtures/micropython.elf'
expected = (elf.with_suffix('.sha256')).read_text().split()[0]
data = elf.read_bytes()
assert hashlib.sha256(data).hexdigest() == expected
assert data[:8] == b'\x7fELF\x02\x01\x01\x03'
assert struct.unpack_from('<HH', data, 16) == (2, 62)
phoff = struct.unpack_from('<Q', data, 32)[0]
phent, phnum = struct.unpack_from('<HH', data, 54)
bss = 0
for i in range(phnum):
    typ, flags, off, va, pa, size, mem, align = struct.unpack_from('<IIQQQQQQ', data, phoff+i*phent)
    assert typ not in (2, 3), 'no PT_DYNAMIC or PT_INTERP in static golden'
    if typ == 1:
        assert 0x400000 <= va < va + mem <= 0x7fc000
        assert flags & 3 != 3, 'no W+X load'
        bss += mem - size
assert bss >= 256*1024
asm = subprocess.check_output(['objdump', '-d', str(elf)], text=True)
assert not re.search(r'%(?:[xyz]mm\d+|mm[0-7]|st\()', asm)
for line in asm.splitlines():
    parts = line.split('\t')
    if len(parts) >= 3:
        op = parts[-1].strip().split()[0]
        assert not (op.startswith('f') and op != 'fs') and not op.startswith('v') and op != 'emms', op

def run(args, status, output=None, token=None):
    p = subprocess.run([str(elf), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
    assert p.returncode == status, (args, p.returncode, p.stdout, p.stderr)
    assert not p.stderr
    if output is not None:
        assert p.stdout == output.encode(), p.stdout
    if token:
        assert token.encode() in p.stdout, p.stdout

run(['-c', "print('micropython inline'); print(6*7); assert 6*7==42"], 0, 'micropython inline\n42\n')
run(['-c', "import sys; print('micropython exit7'); sys.exit(7)"], 7, 'micropython exit7\n')
run([str(root/'user/linux/micropython-demo.py')], 0, 'micropython golden\n42\n')
run(['-c', 's=0\nfor i in range(4000):\n a=[i]*32\n s+=i\nassert s==7998000\nprint(s)\nprint(2**80)'], 0, '7998000\n1208925819614629174706176\n')
run(['-c', 'if :'], 1, token='SyntaxError: invalid syntax')
run(['-c', 'a=[0]*1000000'], 1, token='MemoryError: memory allocation failed')
run(['-c', 'def f():\n return f()\nf()'], 1, token='maximum recursion depth exceeded')
run(['-c', 'print(1.5)'], 1, token='decimal numbers not supported')
run(['-c', "import socket"], 1, token='ImportError:')
run([], 64, token='usage: micropython.elf')
with tempfile.TemporaryDirectory(prefix='capmix-mp-host-') as d:
    path = Path(d)/'script.py'
    run([str(path)], 66, 'script open failed\n')
    path.write_bytes(b'x'*8193)
    run([str(path)], 65, 'script too large\n')
    path.write_bytes(b"print('hidden')\x00\n")
    run([str(path)], 65, 'script contains NUL\n')
print(f'MicroPython static scalar fixture: PASS sha256={expected} (host execution, bounds, errno/status, no SIMD/FP)')
