#!/usr/bin/env python3
"""Validate bounded static clone/futex probes and the existing Linux/native chain."""
import argparse
from pathlib import Path
import subprocess
import re
import hashlib
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
for name in ('disabled','hosted','interp-off','wire'):
    p.add_argument('--'+name,action='store_true')
a=p.parse_args()
flags=['--'+n for n in ('disabled','hosted','interp-off','wire') if getattr(a,n.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_smp.py')),*flags,str(a.log)],check=True)
s=a.log.read_text(errors='replace')
if a.disabled:
    assert 'linux threads skipped: persona disabled' in s
else:
    fixture=Path(__file__).parent/'fixtures/linux-threads.elf'
    expected=(fixture.with_suffix('.sha256')).read_text().split()[0]
    assert hashlib.sha256(fixture.read_bytes()).hexdigest()==expected
    assert f'linux threads fixture: sha256={expected} bytes={fixture.stat().st_size}' in s
    assert 'linux threads selftest: PASS (0 failures)' in s
    assert 'linux threads verified: mode=join exit=0 pages/commit/reap=ok' in s
    if a.hosted:
        assert 'linux threads hosted:' in s
    else:
        for mode,code in [('leader',0),('group',9),('ap',0)]:
            assert f'linux threads verified: mode={mode} exit={code} pages/commit/reap=ok' in s
        assert s.count('linux threads: shared-mm TLS futex join ok')==4
        assert 'linux threads: leader-exit child-live ok' in s
        assert 'linux futex group isolation: cpus=0/1 same-VA separate-mm TLS/WAIT/WAKE/reclaim verified' in s
        assert re.search(r'linux futex wait: tid=\d+ blocked=1',s)
        for result in ('0','-110','-4'):
            assert re.search(r'linux futex resume: tid=\d+ result='+result+r'\b',s)
assert 'selftest result: PASS (0 failures)' in s
print('Linux bounded clone/futex: '+('SKIP persona disabled' if a.disabled else 'PASS'))
