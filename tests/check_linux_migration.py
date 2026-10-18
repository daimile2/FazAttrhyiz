#!/usr/bin/env python3
"""Check migration evidence and existing native/Linux thread/SMP chain."""
import argparse
from pathlib import Path
import re
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('log', type=Path)
for name in ('hosted', 'disabled', 'interp-off', 'wire'):
    p.add_argument('--' + name, action='store_true')
a = p.parse_args()
flags = ['--' + n for n in ('hosted', 'disabled', 'interp-off', 'wire')
         if getattr(a, n.replace('-', '_'))]
subprocess.run(['python3', str(Path(__file__).with_name('check_linux_threads.py')),
                *flags, str(a.log)], check=True)
s = a.log.read_text(errors='replace')
if a.disabled:
    assert 'linux migration skipped: persona disabled' in s
else:
    assert 'linux migration selftest: PASS (0 failures) pages/commit/reap verified' in s
    assert s.count('linux migration endpoint guards: dup=allowed in-flight/foreign-alias/nested=EAGAIN group=EOPNOTSUPP verified') == 2
    if a.hosted:
        assert 'linux migration hosted: validation/group-denied verified; hardware handoff skipped' in s
    else:
        assert s.count('linux migration endpoints: pipe+UDP queued/dup cross-CPU read/write verified') == 2
        if a.wire or 'linux migration TCP: connected endpoint cross-CPU payload verified' in s:
            assert s.count('linux migration TCP: connected endpoint cross-CPU payload verified') == 2
        else:
            assert 'linux migration TCP wire skipped:' in s
        assert s.count('linux migration user: CPU0/1 roundtrip FS/TLS syscall COW ok') == 2
        for rollback in ('not-injected', 'ok'):
            assert ('rounds=4 migrations=8 rollback=' + rollback +
                    ' FS/TLS/COW/syscall/exit=ok') in s
        for source, target in ((0, 1), (1, 0)):
            assert len(re.findall(rf'linux migrate: pid=\d+ from={source} to={target} '
                                  r'rollback=not-needed off-cpu=1 FS=401080', s)) == 8
        assert re.search(r'linux migrate: pid=\d+ from=0 to=0 rollback=ok off-cpu=1', s)
        parents = set(re.findall(r'linux migrate: pid=(\d+) from=0 to=1', s))
        assert len(parents) == 2
        for pid in parents:
            assert re.search(r'linux L2 fork: parent=' + pid + r' child=\d+ .* cpu=1', s)
            assert re.search(r'linux L2 wait4 reap: parent=' + pid + r' child=\d+ status=700', s)
assert 'selftest result: PASS (0 failures)' in s
assert 'migration timeout' not in s
print('Linux controlled migration: ' + ('SKIP disabled' if a.disabled else 'PASS'))
