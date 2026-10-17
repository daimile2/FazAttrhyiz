#!/usr/bin/env python3
"""Require guest output/status/reclaim evidence, ENOSYS=0, and prior golden chain."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
for name in ('hosted','disabled','interp-off','wire'):
    p.add_argument('--'+name,action='store_true')
a=p.parse_args()
flags=['--'+n for n in ('hosted','disabled','interp-off','wire') if getattr(a,n.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_migration.py')),*flags,str(a.log)],check=True)
s=a.log.read_text(errors='replace')
if a.disabled:
    assert 'micropython skipped: persona disabled; native programs retained' in s
else:
    elf=Path(__file__).parent/'fixtures/micropython.elf'
    expected=(elf.with_suffix('.sha256')).read_text().split()[0]
    assert hashlib.sha256(elf.read_bytes()).hexdigest()==expected
    assert f'micropython fixture: version=1.24.1 static-scalar sha256={expected} bytes={elf.stat().st_size}' in s
    assert 'micropython selftest: PASS (0 failures)' in s
    if a.hosted:
        assert 'micropython hosted: ELF/persona/BSS/commit/reclaim verified; guest execution skipped' in s
    else:
        for case,status in [('inline',0),('file',0),('exit7',7),('gc-integer',0),('syntax',1),('heap-limit',1),('stack-limit',1),('missing-file',66),('float-disabled',1),('import-disabled',1),('file-limit',65),('NUL-file',65)]:
            assert f'micropython {case}: CPL=3 output verified exit={status} ENOSYS=0' in s
            assert f'micropython reclaim: case={case} pages/commit/fds=ok' in s
        assert 'linux ENOSYS histogram end: scope=micropython total=0' in s
        assert not re.search(r'linux ENOSYS histogram: scope=micropython nr=',s)
assert 'micropython reclaim mismatch' not in s
assert 'selftest result: PASS (0 failures)' in s
print('MicroPython guest: '+('SKIP disabled' if a.disabled else 'PASS'))
