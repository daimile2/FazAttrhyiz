#!/usr/bin/env python3
"""Check Linux-enabled full-selftest ENOSYS text budget and uncapped nr buckets."""
import argparse,re
from collections import Counter
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);p.add_argument('--hosted',action='store_true');a=p.parse_args()
s=a.log.read_text(errors='replace');assert 'selftest result: PASS (0 failures)' in s
rows=re.findall(r'linux ENOSYS: pid=(\d+) nr=(\d+) count=(\d+)',s)
counts=Counter(pid for pid,nr,n in rows)
assert counts and max(counts.values())==32
assert all(int(n)<=32 for pid,nr,n in rows)
assert 'linux ENOSYS budget: text=32 histogram nr=511 count=35 beyond-budget verified' in s
assert 'scope=negative nr=511 count=35' in s and 'scope=all nr=511 count=35' in s
assert re.search(r'scope=negative nr=512 count=\d+ overflow=1',s)
if a.hosted:assert 'scope=poweroff nr=511 count=35' in s
print('release observation: PASS text=32/process; nr=511 count=35 retained beyond text cap; overflow explicit'+('; poweroff nr dump verified' if a.hosted else ''))
