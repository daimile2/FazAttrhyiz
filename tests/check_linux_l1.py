#!/usr/bin/env python3
"""Check L0 + L1 guest logs (hosted skips actual CPL3 applets)."""
import argparse
import subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
p.add_argument('--disabled',action='store_true')
p.add_argument('--hosted',action='store_true')
a=p.parse_args();text=a.log.read_text(errors='replace')
assert 'selftest result: PASS (0 failures)' in text
if a.disabled:
    assert 'linux L1 skipped: persona disabled; native files retained' in text
    assert 'linux L1 busybox ' not in text
    if not a.hosted:
        subprocess.run(['python3',str(Path(__file__).with_name('check_linux_l0.py')),'--disabled',str(a.log)],check=True)
else:
    assert 'linux L1 fd verified:' in text
    assert 'linux L1 selftest: PASS (0 failures)' in text
    if not a.hosted:
        subprocess.run(['python3',str(Path(__file__).with_name('check_linux_l0.py')),str(a.log)],check=True)
        for name,n in [('echo',22),('cat',21),('ls',11)]:
            assert f'linux L1 busybox {name}: CPL=3 output bytes={n} verified exit=0 ENOSYS=0 BSS-heap=65536' in text
print('Linux L1 '+('disabled/native retained' if a.disabled else 'custom musl BusyBox subset + bounded BSS')+': PASS')
