#!/usr/bin/env python3
"""Deterministically mark this repository's static x86-64 golden ELF as Linux."""
from pathlib import Path
import sys
p = Path(sys.argv[1])
b = bytearray(p.read_bytes())
assert b[:7] == b'\x7fELF\x02\x01\x01'
assert int.from_bytes(b[16:18], 'little') == 2
assert int.from_bytes(b[18:20], 'little') == 62
b[7] = 3  # ELFOSABI_LINUX. Never guess persona from executable instructions.
p.write_bytes(b)
