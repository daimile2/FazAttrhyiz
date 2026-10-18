#!/usr/bin/env python3
"""Read-only baseline arithmetic/payload/CPU evidence checks; does not run VMs."""
import argparse
import json
import math
from pathlib import Path
import re
import statistics
p=argparse.ArgumentParser(description=__doc__);p.add_argument('lab',type=Path);a=p.parse_args()
d=json.loads((a.lab/'results.json').read_text());c=d['config'];rows=d['samples']
assert len(rows)==len(c['cpus'])*c['samples']
for cpu in c['cpus']:
    s=(a.lab/f'smp{cpu}-serial.log').read_text(errors='replace')
    assert re.search(r'\bcpu_count='+str(cpu)+r'\b',s)
    if cpu>1:assert f'online={cpu}/{cpu}' in s
    assert 'root=disk' in s and 'boot mode: product' in s
    assert 'persona: compiled=1 enabled=1' in s
    assert f'httpd handled={8+c["samples"]*c["http_requests"]}' in s
    assert s.count('httpd response: HTTP/1.0 200 payload=25 verified')==8+c['samples']*c['http_requests']
    assert s.count('perf pipe verified bytes='+str(c['pipe_total_bytes']))==c['samples']+1
    assert 'shell error=' not in s and '[FAULT]' not in s and '*** PANIC' not in s
    r=[x for x in rows if x['smp']==cpu]
    assert sorted(x['sample'] for x in r)==list(range(1,c['samples']+1))
    for x in r:
        assert x['http_requests']==c['http_requests'] and x['hush_bytes']==c['pipe_total_bytes']==c['pipes']*6000
        assert 0<x['http_seconds']<40 and 0<x['hush_seconds']<60
        for rate,expected in ((x['http_rps'],x['http_requests']/x['http_seconds']),
                              (x['hush_Bps'],x['hush_bytes']/x['hush_seconds'])):
            assert math.isfinite(rate) and math.isclose(rate,expected,rel_tol=1e-12)
    print(f'PERF_SUMMARY date={c["date"]} backend={c["backend"]} smp={cpu} nic={c["nic"]} linux={c["linux"]} '
          f'samples={c["samples"]} http_rps_median={statistics.median(x["http_rps"] for x in r):.3f} '
          f'hush_KiBps_median={statistics.median(x["hush_Bps"]/1024 for x in r):.2f} verified=1')
print('performance evidence: PASS (read-only arithmetic/CPU/payload assertions, no VMs launched)')
