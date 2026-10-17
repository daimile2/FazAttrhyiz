#!/usr/bin/env python3
"""Verify retained evidence integrity and repository evidence links; does not run VMs."""
import hashlib
import json
from pathlib import Path
import re

base = Path(__file__).resolve().parent
manifest = json.loads((base / 'manifest.json').read_text())
for entry in manifest['files']:
    data = (base / entry['archive']).read_bytes()
    assert hashlib.sha256(data).hexdigest() == entry['archive_sha256'], entry['archive']
    text = data.decode()
    assert not re.search(r'/home/|/Users/|/tmp/|\x1b', text), entry['archive']
    assert ('selftest result: PASS (0 failures)' in text) == entry['full_selftest_pass_marker'], entry['archive']
root = base.parent.parent
count = 0
for doc in [base / 'README.md', *[root / name for name in sorted({d for e in manifest['files'] for d in e['documents']})]]:
    for target in re.findall(r'\]\(([^)]+)\)', doc.read_text()):
        if 'tests/evidence/' in target or (doc == base / 'README.md' and not re.match(r'https?://', target)):
            assert (doc.parent / target.split('#')[0]).exists(), (str(doc), target)
            count += 1
print(f'evidence verification: PASS ({len(manifest["files"])} log hashes, {count} repository links; verification only, no VMs launched by this checker)')
