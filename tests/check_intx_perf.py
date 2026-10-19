#!/usr/bin/env python3
"""Validate matched native VBox ICH9 A/B runs, then print median comparisons."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('poll',type=Path);p.add_argument('irq',type=Path);a=p.parse_args()
for lab in (a.poll,a.irq):
    subprocess.run(['python3',str(Path(__file__).with_name('check_perf_baseline.py')),str(lab)],check=True)
before=json.loads((a.poll/'results.json').read_text());after=json.loads((a.irq/'results.json').read_text())
b,c=before['config'],after['config']
for key in ('backend','chipset','cpus','memory_mib','nic','linux','linux_interp','http_only',
            'samples','http_requests','http_warmup','kernel_sha256'):
    assert b[key]==c[key],key
assert c['backend']=='vbox' and c['chipset']=='ich9' and c['linux']==0 and c['http_only']
assert b['e1000_irq']=='poll' and c['e1000_irq']=='auto'
for cpu in c['cpus']:
    def median(run,key):return statistics.median(x[key] for x in run['samples'] if x['smp']==cpu)
    old,new=median(before,'http_rps'),median(after,'http_rps')
    print(f'INTX_COMPARE date={c["date"]} smp={cpu} linux=0 poll_rps={old:.3f} irq_rps={new:.3f} delta_pct={(new/old-1)*100:.2f} '
          f'poll_p50_ms={median(before,"http_p50_ms"):.3f} irq_p50_ms={median(after,"http_p50_ms"):.3f} '
          f'poll_p95_ms={median(before,"http_p95_ms"):.3f} irq_p95_ms={median(after,"http_p95_ms"):.3f}')
print('INTx A/B evidence: PASS (same kernel/config/workload; IRQ RX observed; no speedup assertion)')
