#!/usr/bin/env python3
"""Mark our RIP-relative, relocation-free L4a probe as Linux ET_DYN."""
from pathlib import Path
import struct
import sys
p=Path(sys.argv[1]);b=bytearray(p.read_bytes())
assert b[:7]==b'\x7fELF\x02\x01\x01'
assert struct.unpack_from('<HH',b,16)==(2,62)
# A hand-written PIC fixture linked at zero; no relocation tables are emitted.
phoff=struct.unpack_from('<Q',b,32)[0]
entsize,count=struct.unpack_from('<HH',b,54)
assert entsize==56 and count==1
assert struct.unpack_from('<I',b,phoff)[0]==1
assert struct.unpack_from('<Q',b,phoff+16)[0]==0
shoff=struct.unpack_from('<Q',b,40)[0]
shent,shnum=struct.unpack_from('<HH',b,58)
for i in range(shnum):
    assert struct.unpack_from('<I',b,shoff+i*shent+4)[0] not in (4,9), 'relocations unsupported'
b[7]=3;struct.pack_into('<H',b,16,3)
p.write_bytes(b)
