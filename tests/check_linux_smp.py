#!/usr/bin/env python3
"""Assert actual Linux SMP lifecycle/overlap evidence, plus the prior chain."""
import argparse
from pathlib import Path
import re
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
for n in ('hosted','disabled','interp-off','wire'):
    p.add_argument('--'+n,action='store_true')
a=p.parse_args()
flags=['--'+n for n in ('hosted','disabled','interp-off','wire') if getattr(a,n.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_tcp_pool.py')),*flags,str(a.log)],check=True)
s=a.log.read_text(errors='replace')
if a.disabled:
    assert 'linux SMP skipped: persona disabled native affinity unchanged' in s
elif a.hosted:
    assert 'linux SMP hosted: AP hardware execution skipped' in s
else:
    assert 'linux SMP selftest: PASS (0 failures) COW/commit/pages/reap verified' in s
    for cpu in range(4):
        assert f'linux SMP AP lifecycle: cpu={cpu} mode=FS-getcpu-affinity create/run/reap=ok' in s
    assert 'linux SMP overlap: cpus=4 pair=1/2 overlap=1 getcpu/affinity/FS verified' in s
    assert 'linux SMP AP lifecycle: cpu=1 mode=fork-COW-exec-wait create/run/reap=ok' in s
    assert re.search(r'linux L2 fork: parent=\d+ child=\d+ pages=\d+ COW=RO reserves=resident cpu=1',s)
assert 'selftest result: PASS (0 failures)' in s
print('Linux SMP: '+('SKIP (gate/hosted)' if a.disabled or a.hosted else 'PASS (4 CPUs/AP lifecycle/overlap/COW/exec/reap)'))
