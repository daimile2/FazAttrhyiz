#!/usr/bin/env python3
"""Audit pinned x86-64 initial-exec TLS; constants from system elf.h/SysV ABI."""
from pathlib import Path
import struct,sys,subprocess
root=Path(sys.argv[1])
for name in ('linux-tls.elf','libcm_tls_a.so.1','libcm_tls_b.so.1'):
    b=(root/name).read_bytes();assert b[:8]==b'\x7fELF\x02\x01\x01\x03'
    off=struct.unpack_from('<Q',b,32)[0];ent,n=struct.unpack_from('<HH',b,54);assert ent==56 and n<=16
    ph=[struct.unpack_from('<IIQQQQQQ',b,off+i*ent) for i in range(n)]
    tls=[p for p in ph if p[0]==7]
    if name=='linux-tls.elf':assert not tls
    else:
        assert len(tls)==1
        t=tls[0];assert 0<t[5]<t[6]<=65536 and t[7]==64 and t[3]%64==0
        rel=subprocess.check_output(['readelf','-rW',str(root/name)],text=True)
        assert rel.count('R_X86_64_TPOFF64')==2 and '__tls_get_addr' not in rel
        assert 'DTPMOD' not in rel and 'TLSDESC' not in rel
    dis=subprocess.check_output(['objdump','-d',str(root/name)],text=True)
    assert not any(r in dis for r in ('%xmm','%ymm','%zmm'))
print('fixed TLS ABI: two IE DSOs, TPOFF64=4, align=64, initialized/.tbss, no main/interpreter TLS')
