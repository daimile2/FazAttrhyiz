#!/usr/bin/env python3
"""Assert saved freestanding Linux L0 enabled/disabled regression logs."""
import argparse
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('log', type=Path)
p.add_argument('--disabled', action='store_true')
a = p.parse_args()
text = a.log.read_text(errors='replace').replace('\r\n', '\n')
assert 'selftest result: PASS (0 failures)' in text
assert 'linux L0 selftest: PASS (0 failures)' in text
assert 'sched IPC background joined: work=201 stack/pages reclaimed' in text
reclaim = re.search(r'linux L0 reclaim verified: before=(\d+) after=(\d+)', text)
assert reclaim and reclaim[1] == reclaim[2], 'L0 page baseline must be exactly restored'
if a.disabled:
    assert 'linux L0 disabled verified:' in text
    assert 'linux persona disabled:' in text
    assert 'linux golden verified: CPL=3' not in text
else:
    assert '\nhello from Linux ABI L0\n' in text
    assert 'linux golden verified: CPL=3 write_bytes=24 exit=0 ENOSYS=0' in text
    assert 'linux FS/auxv verified: same-cpu overlap' in text
    assert 'scheduler_FS=0 ENOSYS=2/2' in text
    smp_probes=len(re.findall(r'linux SMP AP lifecycle: cpu=\d+ mode=FS-getcpu-affinity create/run/reap=ok',text))
    assert text.splitlines().count('linux auxv/fs isolation ok') == 2+smp_probes
assert 'PT_INTERP dynamic loader unsupported' in text or 'elf rejected: malformed PT_INTERP' in text
print(f'Linux L0 {"disabled/native retained" if a.disabled else "golden/auxv/FS/ENOSYS"}: PASS')
