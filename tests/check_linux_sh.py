#!/usr/bin/env python3
"""Validate fixed BusyBox hush results, errors, reclamation and L0–L5 regressions."""
import argparse
import re
import subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
for name in ('disabled','interp-off','hosted','wire'):
    p.add_argument('--'+name,action='store_true')
a=p.parse_args()
flags=['--'+n for n in ('disabled','interp-off','hosted','wire') if getattr(a,n.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_l5.py')),*flags,str(a.log)],check=True)
s=a.log.read_text(errors='replace')
if a.disabled:
    assert 'linux sh skipped: persona disabled; native shell retained' in s
else:
    assert 'linux sh selftest: PASS (0 failures)' in s
    assert 'linux sh pipe mechanism: cap rights/dup/EOF/EPIPE/EAGAIN/EFAULT/capacity=8 cleanup verified' in s
    assert 'linux sh reclaim mismatch' not in s
    if not a.hosted:
        for name,code in [('basic',0),('subshell',0),('exec',0),('pipe',0),('stream',0),('missing',127),('exit',7)]:
            assert f'linux sh {name}: CPL=3 output verified exit={code}' in s
            state=re.search(rf'linux sh state {name}: exited=1 tid=0 code={code} last=\d+ calls=\d+ ENOSYS=(\d+)',s)
            assert state and int(state[1])<=32
        assert 'linux sh captured stream: bytes=6000' in s
        assert "sh: can't execute '/missing-command': No such file or directory" in s
print('Linux fixed musl BusyBox sh '+('disabled/native retained' if a.disabled else 'commands/fork/exec/pipe/error/reclaim')+': PASS')
