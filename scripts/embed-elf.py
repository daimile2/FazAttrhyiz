#!/usr/bin/env python3
"""Embed the built fixture; the kernel writes these exact ELF bytes via VFS."""
import pathlib
import sys
source, destination = map(pathlib.Path, sys.argv[1:3])
symbol = sys.argv[3] if len(sys.argv) > 3 else 'cm_test_elf'
if not symbol.isidentifier():
    raise SystemExit('invalid C array identifier')
data = source.read_bytes()
lines = [f'/* Generated from {source}; do not edit. */',
         f'static const unsigned char {symbol}[] = {{']
for offset in range(0, len(data), 16):
    lines.append('    ' + ', '.join(f'0x{value:02x}' for value in data[offset:offset + 16]) + ',')
lines.append('};')
destination.write_text('\n'.join(lines) + '\n')
