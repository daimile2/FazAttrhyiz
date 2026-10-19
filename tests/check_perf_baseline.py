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
    assert 'persona: compiled=1 enabled='+str(c['linux']) in s
    total=8+c['samples']*c['http_requests'];workers=c.get('workers',1)
    if 'workers' in c:
        quota=total//workers
        for worker in range(workers):assert f'httpd worker done: id={worker+1} port={8080+worker} handled={quota} status=0' in s
    else:assert f'httpd handled={total}' in s
    assert s.count('httpd response: HTTP/1.0 200 payload=25 verified')==8+c['samples']*c['http_requests']
    if not c.get('http_only'):assert s.count('perf pipe verified bytes='+str(c['pipe_total_bytes']))==c['samples']+1
    if c.get('e1000_irq')=='poll':assert 'forced_poll=1' in s and 'e1000 irq rx enabled:' not in s
    if c.get('chipset')=='ich9' and c['backend']=='vbox' and c.get('e1000_irq')=='auto':
        assert 'verified_gsi=' in s and 'e1000 irq rx enabled:' in s
        assert re.search(r'net irq rx active: if=enet0 hardware=[1-9]\d* notified-frame=1',s)
    assert 'shell error=' not in s and '[FAULT]' not in s and '*** PANIC' not in s
    r=[x for x in rows if x['smp']==cpu]
    assert sorted(x['sample'] for x in r)==list(range(1,c['samples']+1))
    for x in r:
        assert x['http_requests']==c['http_requests'] and 0<x['http_seconds']<40
        rates=[(x['http_rps'],x['http_requests']/x['http_seconds'])]
        if c.get('http_only'):
            assert c['linux']==0 and x['hush_bytes']==0 and x['hush_seconds'] is None and x['hush_Bps'] is None
        else:
            assert x['hush_bytes']==c['pipe_total_bytes']==c['pipes']*6000 and 0<x['hush_seconds']<60
            rates.append((x['hush_Bps'],x['hush_bytes']/x['hush_seconds']))
        if 'workers' in c:
            placements=x['worker_placements'];assert len(placements)==workers
            assert len({v['pid'] for v in placements})==workers
            assert all(0<=v['cpu']<cpu and v['port']==8079+v['worker'] for v in placements)
            if cpu>1 and workers>1:assert len({v['cpu'] for v in placements})>1
            for v in placements:assert f'shell child done: pid={v["pid"]} exit=0 reap=ok' in s
            assert 1<=x['client_peak']<=c['concurrency']
            if c['concurrency']>1:assert x['client_peak']>1
        if 'http_latency_ms' in x:
            lat=x['http_latency_ms'];assert len(lat)==x['http_requests'] and all(0<t<3000 for t in lat)
            assert math.isclose(x['http_p50_ms'],statistics.median(lat))
            assert math.isclose(x['http_p95_ms'],sorted(lat)[math.ceil(.95*len(lat))-1])
        for rate,expected in rates:
            assert math.isfinite(rate) and math.isclose(rate,expected,rel_tol=1e-12)
    print(f'PERF_SUMMARY date={c["date"]} backend={c["backend"]} smp={cpu} nic={c["nic"]} linux={c["linux"]} '
          f'workers={c.get("workers",1)} concurrency={c.get("concurrency",1)} samples={c["samples"]} http_rps_median={statistics.median(x["http_rps"] for x in r):.3f} '
          + ('hush=skipped-linux-off verified=1' if c.get('http_only') else f'hush_KiBps_median={statistics.median(x["hush_Bps"]/1024 for x in r):.2f} verified=1'))
    if 'http_p50_ms' in r[0]:print(f'LATENCY smp={cpu} p50_ms={statistics.median(x["http_p50_ms"] for x in r):.3f} p95_ms={statistics.median(x["http_p95_ms"] for x in r):.3f}')
print('performance evidence: PASS (read-only arithmetic/CPU/payload assertions, no VMs launched)')
