#!/usr/bin/env python3
"""Check dynamic W^X/RELRO subset plus TLS and the static/native chain."""
import argparse,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
for n in ('disabled','interp-off','hosted','wire'):p.add_argument('--'+n,action='store_true')
a=p.parse_args();flags=['--'+n for n in ('disabled','interp-off','hosted','wire') if getattr(a,n.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_tls.py')),*flags,str(a.log)],check=True)
s=a.log.read_text(errors='replace')
if not (a.disabled or a.interp_off):
    for marker in ('linux W^X mechanism verified: RWX=EPERM range-atomic RX/RO copyout denied shared=EOPNOTSUPP COW NX retained','linux W^X mechanism rollback/reclaim=ok'):
        assert marker in s
    assert 'linux W^X loader protocol verified: stale interpreter rejected rollback=ok' in s
    assert 'linux W^X reclaim mismatch' not in s
    if not a.hosted:
        assert s.count('ldso: W^X active objects=3 RELRO=3 no-relro=0')==2
        assert s.count('ldso: W^X active objects=4 RELRO=4 no-relro=0')==3
        assert 'ldso: W^X active objects=3 RELRO=2 no-relro=1' in s
        assert 'ldso: reject W+X PT_LOAD' in s and 'ldso: reject RELRO bounds/alignment' in s
        assert s.count('linux W^X/RELRO PTE verified:')==4
print('Linux dynamic W^X/RELRO: '+('SKIP (gate)' if a.disabled or a.interp_off else 'PASS'))
