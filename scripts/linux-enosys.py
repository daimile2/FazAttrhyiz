#!/usr/bin/env python3
"""Run a fixed Linux-enabled selftest ISO, validate and rank exact ENOSYS buckets.
Default NIC=none avoids host echo service dependencies. No guest disk is attached.
"""
import argparse
import hashlib
from collections import Counter
from pathlib import Path
import re
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso', type=Path, default=Path('capmix.iso'))
p.add_argument('--log', type=Path, default=Path('/tmp/capmix-enosys.log'))
p.add_argument('--report', type=Path)
p.add_argument('--parse-only', action='store_true')
p.add_argument('--legacy', action='store_true', help='read truncated old per-process lines; lower bounds only')
p.add_argument('--scope', choices=('all', 'hush', 'negative', 'micropython'), default='hush')
p.add_argument('--nic', choices=('none', 'e1000', 'virtio-net-pci'), default='none')
p.add_argument('--dual', action='store_true')
p.add_argument('--disabled', action='store_true')
p.add_argument('--timeout', type=int, default=30)
a = p.parse_args()
if not a.parse_only:
    for manifest in ('tests/fixtures/linux-l0.sha256', 'tests/fixtures/busybox-l1.sha256',
                     'tests/fixtures/busybox-sh.sha256', 'tests/fixtures/micropython.sha256', 'tests/fixtures/ldso/SHA256SUMS'):
        for line in Path(manifest).read_text().splitlines():
            if not line.strip() or line.startswith('#'):
                continue
            expected, filename = line.split(None, 1)
            assert hashlib.sha256(Path(filename.strip()).read_bytes()).hexdigest() == expected, filename
    print('fixed fixture hashes verified; iso sha256=' + hashlib.sha256(a.iso.read_bytes()).hexdigest(), flush=True)
    cmd = ['qemu-system-x86_64', '-enable-kvm', '-m', '512', '-cpu', 'host', '-smp', '4',
           '-cdrom', str(a.iso), '-serial', 'stdio', '-display', 'none', '-no-reboot', '-d', 'cpu_reset']
    if a.nic == 'none':
        cmd += ['-nic', 'none']
    else:
        cmd += ['-netdev', 'user,id=n0', '-device', a.nic + ',netdev=n0']
    if a.dual:
        cmd += ['-object', 'memory-backend-ram,id=m0,size=256M',
                '-object', 'memory-backend-ram,id=m1,size=256M',
                '-numa', 'node,nodeid=0,memdev=m0,cpus=0-1',
                '-numa', 'node,nodeid=1,memdev=m1,cpus=2-3']
    with a.log.open('w') as out:
        guest = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT)
        try:
            end = time.monotonic() + a.timeout
            while time.monotonic() < end:
                text = a.log.read_text(errors='replace')
                if 'selftest result:' in text or guest.poll() is not None:
                    break
                time.sleep(.1)
        finally:
            if guest.poll() is None:
                guest.terminate()
            try:
                guest.wait(timeout=5)
            except subprocess.TimeoutExpired:
                guest.kill()
                guest.wait()
text = a.log.read_text(errors='replace')
assert 'selftest result: PASS (0 failures)' in text, 'missing full PASS: ' + str(a.log)
counts = Counter()
if a.legacy:
    counts.update(map(int, re.findall(r'linux ENOSYS: pid=\d+ nr=(\d+)', text)))
    label = 'legacy visible lower bounds (all logged probes; per-process truncation)'
else:
    pattern = r'linux ENOSYS histogram: scope=' + a.scope + r' nr=(\d+) count=(\d+)([^\r\n]*)'
    for nr, n, suffix in re.findall(pattern, text):
        key = '>=512 overflow' if 'overflow=1' in suffix else int(nr)
        counts[key] += int(n)
    totals = re.findall(r'linux ENOSYS histogram end: scope=' + a.scope + r' total=(\d+)', text)
    if a.disabled:
        assert 'linux compat skipped: persona disabled' in text
        assert not re.search(r'linux ENOSYS histogram: scope=all ', text)
    else:
        assert 'linux compat nr=13/14:' in text
        assert 'linux compat selftest: PASS (0 failures)' in text
        assert 'linux sh selftest: PASS (0 failures)' in text
    if not (a.disabled and a.scope != 'all'):
        assert len(totals) == 1 and sum(counts.values()) == int(totals[0]), 'missing/partial/duplicate histogram'
    if a.scope in ('hush', 'micropython'):
        assert not counts, 'fixed '+a.scope+' corpus must have ENOSYS=0'
    if a.scope == 'micropython' and not a.disabled:
        assert 'micropython selftest: PASS (0 failures)' in text
    label = a.scope + ' exact buckets (>=512 grouped, not individually ranked)'
names = {13: 'rt_sigaction', 14: 'rt_sigprocmask', 41: 'socket', 54: 'setsockopt', 55: 'getsockopt'}
lines = [label, 'nr\tcount\tname']
for nr, n in sorted(counts.items(), key=lambda x: (-x[1], str(x[0]))):
    lines.append(f'{nr}\t{n}\t{names.get(nr, "unknown/probe")}')
lines.append(f'total\t{sum(counts.values())}')
report = '\n'.join(lines) + '\n'
if a.report:
    a.report.write_text(report)
print(report, end='')
