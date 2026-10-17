#!/usr/bin/env python3
"""Audit the fixed dynamic-linking ABI using ELF program headers, not symbol names."""
from collections import Counter
from pathlib import Path
import struct
import subprocess
import sys
root=Path(sys.argv[1])
counts=Counter()
for name,kind in [('linux-dynamic.elf',2),('libcmgold.so.1',3),('ld-capmix.so.1',3)]:
    path=root/name;b=path.read_bytes();assert b[:8]==b'\x7fELF\x02\x01\x01\x03'
    assert struct.unpack_from('<HH',b,16)==(kind,62)
    off=struct.unpack_from('<Q',b,32)[0];ent,n=struct.unpack_from('<HH',b,54);assert ent==56 and 0<n<=16
    ph=[struct.unpack_from('<IIQQQQQQ',b,off+i*ent) for i in range(n)]
    assert not any(p[0]==7 for p in ph),'TLS not in this target'
    def fileoff(va,size):
        for t,flags,o,v,_,fs,ms,a in ph:
            if t==1 and va>=v and va-v<=fs and size<=fs-(va-v):return o+va-v
        raise AssertionError('dynamic pointer outside file PT_LOAD')
    ds=[p for p in ph if p[0]==2];assert len(ds)==1
    tags={};needed=[];p=ds[0]
    for i in range(p[5]//16):
        t,v=struct.unpack_from('<QQ',b,p[2]+16*i)
        if not t:break
        if t==1:needed.append(v)
        else:assert t not in tags;tags[t]=v
    else:raise AssertionError('unterminated dynamic')
    assert 4 in tags and 0x6ffffef5 not in tags,'SysV hash required'
    assert not any(t in tags for t in (12,13,25,26,32,35,36,37)), 'constructors/RELR unsupported'
    rel=Counter()
    for address,size in ((tags.get(7,0),tags.get(8,0)),(tags.get(23,0),tags.get(2,0))):
        assert size%24==0
        if size:
            at=fileoff(address,size)
            for i in range(size//24):rel[struct.unpack_from('<Q',b,at+24*i+8)[0]&0xffffffff]+=1
    if name=='linux-dynamic.elf':
        assert rel==Counter({6:2,1:1,7:1}),rel
        ip=[p for p in ph if p[0]==3];assert len(ip)==1
        assert b[ip[0][2]:ip[0][2]+ip[0][5]]==b'/lib/ld-capmix.so.1\0'
        assert len(needed)==1
        strings=fileoff(tags[5],tags[10]);at=strings+needed[0]
        assert b[at:b.index(0,at)]==b'libcmgold.so.1'
    elif name=='libcmgold.so.1':assert rel==Counter({8:1,6:2}) and not needed,rel
    else:
        assert not rel and not needed,'interpreter must be self-relocation-free PIC'
        notes=[]
        for p in ph:
            if p[0]!=4:continue
            at=p[2];end=at+p[5]
            while at<end:
                ns,ds,ty=struct.unpack_from('<III',b,at);at+=12
                owner=b[at:at+ns];at+=(ns+3)&~3
                desc=b[at:at+ds];at+=(ds+3)&~3
                if owner==b'CapMixWX\0':notes.append((ty,desc))
        assert notes==[(1,struct.pack('<I',1))], 'hardened loader protocol v1 required'
    counts+=rel
    dis=subprocess.check_output(['objdump','-d',str(path)],text=True)
    assert not any(reg in dis for reg in ('%xmm','%ymm','%zmm')),'no SIMD user context claim'
assert counts==Counter({8:1,6:4,7:1,1:1}),counts
print('fixed ldso ABI: RELATIVE=1 GLOB_DAT=4 JUMP_SLOT=1 R64=1; TLS=none; interpreter=PIC/no-relocs')
