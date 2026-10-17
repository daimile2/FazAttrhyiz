#!/usr/bin/env python3
"""Check the four historical VBox debug failures, without treating SKIP as wire PASS."""
import argparse
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
p.add_argument('--carrier-down',action='store_true')
a=p.parse_args();s=a.log.read_text(errors='replace')
assert 'selftest result: PASS (0 failures)' in s
assert 'selftest: FAIL' not in s and 'selftest result: FAIL' not in s
for marker in ('tcp pool verified: slots=32','tcp RX verified: capacity=16384',
               'tcp pool cleanup verified: live=0 pages/commit unchanged',
               'tcp ELF selftest: PASS (0 failures)', 'p1b selftest: PASS (0 failures)',
               'p1d selftest: PASS (0 failures)', 'udp user ok: CPL=3 lo tx/rx payload verified'):
    assert marker in s,marker
if a.carrier_down:
    for marker in ('e1000 link unavailable: timeout_ms=6000', 'm8 e1000 wire SKIP:',
                   'p1b UDP/bg RX wire SKIP:', 'p1b UDP user ELF SKIP:',
                   'p1d TCP echo/multi wire SKIP:', 'tcp ELF wire SKIP:',
                   'tcp ELF no NIC verified: connect=NOENT skip exit=0 rights denied'):
        assert marker in s,marker
    for forbidden in ('m8 e1000 ring wrap verified:', 'tcp multi verified: if=enet0',
                      'p1b UDP enet0 bg RX verified:', 'tcp ELF syscall verified:'):
        assert forbidden not in s,forbidden
    print('VBox four-assertion closure: explicit carrier-down SKIP; pool/lo/no-NIC ELF PASS (wire untested)')
else:
    for marker in ('e1000 link ready:', 'm8 e1000 ring wrap verified: probes=9',
                   'm8 e1000 selftest: PASS (0 failures)',
                   'p1b UDP enet0 bg RX verified:', 'irq rx verified: enet0 slept=1',
                   'tcp multi verified: if=enet0 simultaneous=8 clients=8 bytes_each=8193',
                   'tcp ELF syscall verified: CPL=3 send/recv=2049'):
        assert marker in s,marker
    assert 'wire SKIP:' not in s
    print('VBox four-assertion closure: M8/P1b/P1d/TCP-ELF wire PASS; multi8/IRQ/lo/reclaim verified')
