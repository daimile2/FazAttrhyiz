#!/usr/bin/env python3
"""Read-only one/four process HTTP comparison with identical concurrent clients."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('single',type=Path);p.add_argument('workers',type=Path);a=p.parse_args()
for lab in (a.single,a.workers):subprocess.run(['python3',str(Path(__file__).with_name('check_perf_baseline.py')),str(lab)],check=True)
runs=[json.loads((lab/'results.json').read_text()) for lab in (a.single,a.workers)]
b,c=[x['config'] for x in runs]
for key in ('backend','chipset','cpus','memory_mib','nic','linux','linux_interp','http_only','samples','http_requests','http_warmup','concurrency','e1000_irq','kernel_sha256'):
    assert b[key]==c[key],key
assert b['workers']==1 and c['workers']==4 and c['concurrency']==4 and c['linux']==0
for run in runs:
    config=run['config'];medians={}
    for cpu in config['cpus']:
        rows=[x for x in run['samples'] if x['smp']==cpu]
        medians[cpu]=statistics.median(x['http_rps'] for x in rows)
        print(f'HTTP_WORKERS backend={config["backend"]} workers={config["workers"]} concurrency=4 smp={cpu} '
              f'rps={medians[cpu]:.3f} p50_ms={statistics.median(x["http_p50_ms"] for x in rows):.3f} '
              f'p95_ms={statistics.median(x["http_p95_ms"] for x in rows):.3f} worker_cpus='+','.join(str(x['cpu']) for x in rows[0]['worker_placements']))
    if 1 in medians and 4 in medians:print(f'HTTP_SMP backend={config["backend"]} workers={config["workers"]} smp4_vs_smp1_pct={(medians[4]/medians[1]-1)*100:.2f}')
print('HTTP workers comparison: PASS (bounded independent ports, exact payload/EOF/quota/exit/reap; no speedup assertion)')
