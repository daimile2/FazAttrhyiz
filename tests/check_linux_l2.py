#!/usr/bin/env python3
"""Validate saved L2 hosted or actual guest regression evidence."""
import argparse
import subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
p.add_argument('--disabled',action='store_true')
p.add_argument('--hosted',action='store_true')
a=p.parse_args();text=a.log.read_text(errors='replace').replace('\r\n','\n')
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_l1.py'))]+(['--disabled'] if a.disabled else [])+(['--hosted'] if a.hosted else [])+[str(a.log)],check=True)
if a.disabled:
    assert 'linux L2 skipped: persona disabled; native fork=NOTSUP' in text
    assert 'linux L2 fork: parent=' not in text
else:
    assert 'linux L2 selftest: PASS (0 failures)' in text
    assert 'nested refs/brk zero/shrink/limit/commit denial verified resident-only swap=excluded' in text
    assert 'linux L2 fork denied FILE DUPLICATE: rollback=ok' in text
    if not a.hosted:
        import re
        # Other mechanism tests may fork before the L2 golden. Correlate its
        # actual launch PID -> fork child -> same-child exec and wait status.
        parents=re.findall(r'exec spawn: pid=(\d+) path=/bin/linux-l2\.elf ELF',text)
        matched=False
        for parent in parents:
            for child in re.findall(r'linux L2 fork: parent='+parent+r' child=(\d+)\b',text):
                if (f'linux L2 execve replace: pid={child} path=/bin/linux-l2-next.elf' in text and
                    f'linux L2 wait4 reap: parent={parent} child={child} status=2a00' in text and
                    re.search(r'linux L2 COW fault: pid=(?:'+parent+'|'+child+r')\b',text)):
                    matched=True
        assert matched, 'L2 golden launch/fork/COW/exec/wait42 chain missing'
        assert '\nlinux L2 exec target argv FS CLOEXEC verified\n' in text
        assert '\nlinux L2 fork COW brk exec wait verified\n' in text
        assert 'linux L2 CPL3 verified:' in text
print('Linux L2 '+('disabled/native retained' if a.disabled else 'COW/commit/brk/exec/reap')+': PASS')
