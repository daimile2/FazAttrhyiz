#!/usr/bin/env python3
"""Check bounded DSO IE TLS and the existing Linux/native regression chain."""
import argparse,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
for name in ('disabled','interp-off','hosted','wire'):p.add_argument('--'+name,action='store_true')
a=p.parse_args();flags=['--'+n for n in ('disabled','interp-off','hosted','wire') if getattr(a,n.replace('-','_'))]
subprocess.run(['python3',str(Path(__file__).with_name('check_linux_ldso.py')),*flags,str(a.log)],check=True)
s=a.log.read_text(errors='replace')
if not (a.disabled or a.interp_off or a.hosted):
    assert 'linux tls selftest: PASS (0 failures)' in s
    assert 'linux tls isolation verified: same-cpu overlap preempts=' in s
    assert 'linux tls overlap rollback/reclaim=ok' in s
    assert s.count('ldso: tls init modules=2 TPOFF64=4')==3
    assert s.count('linux tls: payload verified')==3
    for name in ('tls-golden','tls-bad-template','tls-limit','tls-alignment','tls-general-dynamic'):
        code=0 if name=='tls-golden' else 126
        assert f'linux ldso {name}: CPL3 exit={code} output verified ENOSYS=0 rollback/reclaim=ok' in s
    assert 'linux tls reclaim mismatch' not in s
print('Linux bounded DSO initial-exec TLS: '+('SKIP (gate/hosted)' if a.disabled or a.interp_off or a.hosted else 'PASS'))
