#!/usr/bin/env python3
"""Embed the built fixture; the kernel writes these exact ELF bytes via VFS."""
import pathlib
import sys
source, destination = map(pathlib.Path, sys.argv[1:])
data = source.read_bytes()
lines = ['/* Generated from tests/hello_elf.S; do not edit. */',
         'static const unsigned char cm_test_elf[] = {']
for offset in range(0, len(data), 16):
    lines.append('    ' + ', '.join(f'0x{value:02x}' for value in data[offset:offset + 16]) + ',')
lines.append('};')
destination.write_text('\n'.join(lines) + '\n')
