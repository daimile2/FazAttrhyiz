#!/usr/bin/env python3
"""Validate TCP32 pool/RX mechanisms, wire clients and existing Linux chain."""
import argparse,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
p.add_argument('--rx',type=int,default=16384)
for n in ('disabled','interp-off','hosted','wire'):p.add_argument('--'+n,action='store_true')
a=p.parse_args();flags=['--'+n for n in ('disabled','interp-off','hosted','wire') if getattr(a,n.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_wx.py')),*flags,str(a.log)],check=True)
s=a.log.read_text(errors='replace')
for marker in (f'tcp pool: slots=32 backlog=8 rx_bytes={a.rx}',
 'tcp pool verified: slots=32 full=NOSPC passive-full=NOSPC reuse=ok backlog=8 accepted=8 payload isolated',
 f'tcp RX verified: capacity={a.rx} full=AGAIN no-overwrite wrap/payload verified free-window=0->{a.rx}',
 'tcp pool cleanup verified: live=0 pages/commit unchanged',
 'tcp rto mechanism verified: SYN/data/FIN retained; 200/400/800/1600ms backoff max-retries=4'):
 assert marker in s,marker
if a.wire and not a.hosted:
 for iface in ('vnet0','enet0'):
  if f'tcp payload verified: if={iface}' in s:
   assert f'tcp multi verified: if={iface} simultaneous=8 clients=8 bytes_each=8193 payload isolated close/reclaim=ok' in s
 assert 'tcp multi verified:' in s
assert 'tcp pool memory/commit reclaim mismatch' not in s
assert 'tcp multi memory/commit/slot reclaim mismatch' not in s
print(f'TCP32 pool/RX {a.rx} '+('wire multi8' if a.wire and not a.hosted else 'mechanism')+': PASS')
