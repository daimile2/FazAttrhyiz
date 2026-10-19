#!/usr/bin/env python3
"""Validate measured phases and print comparisons, never fabricate speedup."""
import argparse,json,re,statistics as st,math
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('labs',type=Path,nargs='+');a=p.parse_args()
for lab in a.labs:
 d=json.loads((lab/'diagnosis.json').read_text());c=d['config']
 print(f'CONFIG backend={c["backend"]} serial_only={c.get("serial_only",False)} kernel={c["kernel_sha256"]} date={c["date"]}')
 for run in d['runs']:
  cpu=run['smp'];log=(lab/f'smp{cpu}-serial.log').read_text(errors='replace')
  assert 'shell error=' not in log and 'Resource temporarily unavailable' not in log and 'Broken pipe' not in log
  assert 'scope=poweroff' in log
  groups={}
  for parent,where in re.findall(r'linux L2 fork: parent=(\d+) child=\d+ .* cpu=(\d+)',log):groups.setdefault(parent,set()).add(where)
  assert groups and all(len(v)==1 for v in groups.values()),'fork affinity changed within parent'
  if c.get('serial_only'):assert 'fb console skipped' in log and 'fb console ready:' not in log
  for mode in ('http','paced'):
   batches=run[mode];assert len(batches)==c['samples']
   times=[];parts=[]
   for batch in batches:
    assert len(batch['latency_ms'])==len(batch['phase_ms'])==c['http_requests']
    assert batch['gap_ms']==(130 if mode=='paced' else 0)
    for value,phase in zip(batch['latency_ms'],batch['phase_ms']):
     assert all(math.isfinite(x) and x>=0 for x in phase.values())
     assert math.isclose(value,sum(phase.values()),abs_tol=.00001)
     times.append(value);parts.append(phase)
   print(f'HTTP backend={c["backend"]} smp={cpu} serial_only={int(c.get("serial_only",False))} mode={mode} n={len(times)} p50_ms={st.median(times):.3f} '+ ' '.join(f'{key}_ms={st.median(v[key] for v in parts):.3f}' for key in parts[0]))
  expected=8+2*c['samples']*c['http_requests'];assert len(run['guest_http'])==expected
  print(f'GUEST_HTTP smp={cpu} n={expected} '+' '.join(f'{k}={st.median(v[k] for v in run["guest_http"]):.1f}' for k in ('read_us','send_ack_us','close_us')))
  assert len(run['hush'])==4*c['samples']
  for mode in ('auto','bsp'):
   for workload in ('empty','pipe'):
    rows=[r for r in run['hush'] if r['mode']==mode and r['workload']==workload];assert len(rows)==c['samples']
    assert all(r['host_s']>r['guest_s']>0 and 0<=r['cpu']<cpu for r in rows)
    if mode=='bsp':assert all(r['cpu']==0 for r in rows)
    for where in sorted({r['cpu'] for r in rows}):
     subset=[r for r in rows if r['cpu']==where]
     print(f'HUSH backend={c["backend"]} smp={cpu} serial_only={int(c.get("serial_only",False))} mode={mode} workload={workload} cpu={where} n={len(subset)} guest_s={st.median(r["guest_s"] for r in subset):.6f} host_s={st.median(r["host_s"] for r in subset):.6f} outside_guest_ms={st.median((r["host_s"]-r["guest_s"])*1000 for r in subset):.3f}')
 print('performance investigation: PASS (phase sums, payload/EOF, guest timer, affinity and poweroff evidence; no speedup threshold)')
