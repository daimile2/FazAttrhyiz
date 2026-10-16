#!/usr/bin/env python3
"""Check L4a interpreter handoff or gate-off plus all L0–L3 regressions."""
import argparse
import re
import subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
p.add_argument('--disabled',action='store_true',help='Linux persona disabled')
p.add_argument('--interp-off',action='store_true',help='Linux enabled, interpreter gate disabled')
p.add_argument('--hosted',action='store_true')
p.add_argument('--wire',action='store_true')
a=p.parse_args();text=a.log.read_text(errors='replace').replace('\r\n','\n')
flags=(['--disabled'] if a.disabled else [])+(['--hosted'] if a.hosted else [])+(['--wire'] if a.wire else [])
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_l3.py')),*flags,str(a.log)],check=True)
assert 'sched IPC background joined: work=201 stack/pages reclaimed' in text
m=re.search(r'linux L0 reclaim verified: before=(\d+) after=(\d+)',text)
assert m and m[1]==m[2]
assert 'linux L4a selftest: PASS (0 failures)' in text
if a.disabled or a.interp_off:
    assert 'linux L4a gate-off verified: PT_INTERP rejected native/static retained' in text
    assert 'linux L4a interp mapped:' not in text
else:
    assert 'malformed/overlap/capacity/commit/missing rollback=ok' in text
    ready=re.search(r'main_entry=([0-9a-f]+) initial_rip=([0-9a-f]+) AT_BASE=([0-9a-f]+) handoff-only',text)
    assert ready and 0x400000<=int(ready[1],16)<0x600000<=int(ready[2],16)<0x700000 and int(ready[3],16)==0x600000
    if not a.hosted:
        events=re.findall(r'^linux L4a (interp entered: CPL3 auxv verified|main entered: hello via interpreter)$',text,re.M)
        assert events==['interp entered: CPL3 auxv verified','main entered: hello via interpreter']*2, 'spawn and exec must both enter interpreter before main'
        for mode,code in [('/bin/linux-l4a.elf',0),('/bin/linux-l4a-exec.elf',0),('exec-failure-old-image',42)]:
            assert f'linux L4a CPL3 {mode} verified: exit={code} ENOSYS=0 pages/commit reclaimed' in text
        assert 'linux L4a exec failure: old image survived' in text
print('Linux L4a '+('gate-off/static retained' if a.disabled or a.interp_off else 'interpreter handoff/auxv/rollback')+': PASS')
