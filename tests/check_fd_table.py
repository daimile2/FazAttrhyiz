#!/usr/bin/env python3
"""Require FD64 capacity/reuse evidence plus the existing Linux golden chain."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path)
for flag in ('hosted','disabled','interp-off'):p.add_argument('--'+flag,action='store_true')
a=p.parse_args();s=a.log.read_text(errors='replace')
flags=['--'+x for x in ('hosted','disabled','interp-off') if getattr(a,x.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_sigchld.py')),*flags,str(a.log)],check=True)
assert 'fd64 native: slots=64 filled=61 full=NOSPC rollback=ok high-fd=63 hole=47 reuse=3 verified' in s
assert 'fd64 selftest: PASS (0 failures)' in s
if a.disabled:assert 'fd64 Linux skipped: persona disabled; native tested' in s
else:assert 'fd64 Linux: slots=64 filled=61 full=EMFILE rollback=ok high-fd=63 hole=47 reuse=3 verified' in s
assert 'fd64 metadata: proc_bytes=912 slots=64 fixed_pool=32 delta=192 bytes/slot,6144 bytes/pool' in s
assert 'fd64 reclaim mismatch' not in s
print('FD64 capacity/full-error/rollback/high-fd/reuse/reclaim: PASS'+(' (Linux skipped; native verified)' if a.disabled else ''))
