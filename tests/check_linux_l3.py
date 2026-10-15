#!/usr/bin/env python3
"""Validate L3 saved logs plus the L0–L2 regression chain."""
import argparse
import subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
p.add_argument('--disabled',action='store_true')
p.add_argument('--hosted',action='store_true')
p.add_argument('--wire',action='store_true',help='require actual TCP echo and fork/exec tests')
a=p.parse_args()
s=a.log.read_text(errors='replace')
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_l2.py'))]+(['--disabled'] if a.disabled else [])+(['--hosted'] if a.hosted else [])+[str(a.log)],check=True)
if a.disabled:
    assert 'linux L3 skipped: persona disabled' in s
else:
    assert 'linux L3 selftest: PASS (0 failures)' in s
    assert 'ENOSYS=2 observed; fd/backend capacity bounded' in s
    if not a.hosted:
        assert 'linux L3 CPL3 udp verified: exit=0 ENOSYS=0 socket/commit/pages reclaimed' in s
    if a.wire:
        for mode in ('echo','inherit'):
            assert f'linux L3 CPL3 {mode} verified: exit=0 ENOSYS=0 socket/commit/pages reclaimed' in s
        assert 'linux L3 exec CLOEXEC socket verified; non-CLOEXEC stream retained' in s
print('Linux L3 '+('disabled/native retained' if a.disabled else 'socket/fd/capability')+': PASS')
