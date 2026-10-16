#!/usr/bin/env python3
"""Check L5 mmap/COW-commit logs and the unchanged L0–L4 regression chain."""
import argparse
import subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
for flag in ('disabled','interp-off','hosted','wire'):
    p.add_argument('--'+flag,action='store_true')
a=p.parse_args()
flags=[f'--{n}' for n in ('disabled','interp-off','hosted','wire') if getattr(a,n.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_l4.py')),*flags,str(a.log)],check=True)
s=a.log.read_text(errors='replace')
if a.disabled:
    assert 'linux L5 skipped: persona disabled; native ABI retained' in s
else:
    for marker in ('linux L5 selftest: PASS (0 failures)',
                   'window=16MiB quota=8MiB touched=2048-image pages zero/limit/partial-unmap verified',
                   'linux L5 COW commit: reserve charged, split charge unchanged',
                   'linux L5 partial allocation failure: data/PT/descriptor/commit rollback=ok',
                   'linux L5 reclaim: pages/commit/backings/reserves baseline restored'):
        assert marker in s,marker
    if not a.hosted:
        for marker in ('linux L5 user mmap: 1MiB touch + COW fork + unmap ok',
                       'linux L5 musl1.2.5: malloc=2MiB mmap=1MiB verified',
                       'linux L5 CPL3 mmap/fork/munmap + musl allocator verified: exit=0 ENOSYS=0'):
            assert marker in s,marker
print('Linux L5 '+('disabled/native retained' if a.disabled else 'mmap/window/COW reserve commit/reclaim')+': PASS')
