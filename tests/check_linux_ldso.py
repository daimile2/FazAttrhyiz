#!/usr/bin/env python3
"""Check real bounded rtld plus the L0–L5/hush regression chain."""
import argparse
import subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
for name in ('disabled','interp-off','hosted','wire'):p.add_argument('--'+name,action='store_true')
a=p.parse_args()
flags=['--'+n for n in ('disabled','interp-off','hosted','wire') if getattr(a,n.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_sh.py')),*flags,str(a.log)],check=True)
s=a.log.read_text(errors='replace')
assert 'linux ldso selftest: PASS (0 failures)' in s
assert 'linux ldso reclaim mismatch' not in s
if a.disabled or a.interp_off:
    assert 'linux ldso gate-off: dynamic main rejected; native/static retained' in s
    assert 'linux ldso interp mapped:' not in s
else:
    assert 'linux ldso kernel validation: malformed PT_DYNAMIC/TLS/missing interpreter rollback=ok' in s
    assert 'linux ldso interp mapped: path=/lib/ld-capmix.so.1 base=600000' in s
    if not a.hosted:
        for name,code in [('golden',0),('missing-library',126),('unsupported-reloc',126),('text-reloc',126),('unresolved',126),('TLS-library',126),('restored',0)]:
            assert f'linux ldso {name}: CPL3 exit={code} output verified ENOSYS=0 rollback/reclaim=ok' in s
        assert s.count('ldso: reloc RELATIVE=1 GLOB_DAT=4 JUMP_SLOT=1 R64=1')==2
        for marker in ('ldso: unsupported relocation=37','ldso: unresolved symbol gold_value','ldso: reject TLS unsupported'):
            assert marker in s
print('Linux bounded real ldso '+('gate-off/static retained' if a.disabled or a.interp_off else 'RELA/symbol/DSO/negative/reclaim')+': PASS')
