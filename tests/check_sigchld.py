#!/usr/bin/env python3
"""SIGCHLD evidence, including preserved negative signal/ENOSYS checks."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
for option in ('hosted','disabled','interp-off'):p.add_argument('--'+option,action='store_true')
a=p.parse_args();s=a.log.read_text(errors='replace')
flags=['--'+option for option in ('hosted','disabled','interp-off') if getattr(a,option.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_micropython.py')),*flags,str(a.log)],check=True)
if a.disabled:
    assert 'SIGCHLD selftest skipped: persona disabled' in s
else:
    assert 'SIGCHLD selftest: PASS (0 failures)' in s
    assert 'SIGCHLD mechanism: DFL waitable + blocked pending; IGN/SA_NOCLDWAIT ECHILD; off-CPU autoreap verified' in s
    assert 'linux SIGCHLD autoreap:' in s
    assert 'linux compat accounting: helper nr=41 + unknown overflow counted once verified' in s
    if a.hosted:
        assert 'SIGCHLD hosted: lifecycle mechanism verified; CPL3 fixture execution skipped' in s
    else:
        for marker in ('SIGCHLD guest: blocked pending/coalesce default-wait/status unblock-ignore verified',
                       'SIGCHLD guest: IGN/NOCLDWAIT ECHILD autoreap verified; handler rejected sigreturn=ENOSYS',
                       'SIGCHLD CPL3 fork/wait/pending/auto-reap: PASS; sigreturn ENOSYS counted=1'):
            assert marker in s,marker
        assert 'nr=15 count=1' in s
assert 'selftest result: PASS (0 failures)' in s
print('SIGCHLD evidence: '+('SKIP disabled; native regression PASS' if a.disabled else 'PASS (mechanism, negative errno/ENOSYS preserved)'))
