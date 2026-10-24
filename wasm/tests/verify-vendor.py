#!/usr/bin/env python3
"""Verify snapshot hashes; optionally compare with the current parent checkout."""
from pathlib import Path
import argparse
import hashlib
import json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--checkout', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / 'vendor/source.json').read_text())
for name, entry in manifest['files'].items():
    data = (root / 'vendor' / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != entry['sha256']:
        raise SystemExit('Snapshot hash mismatch: ' + name)
    if args.checkout and data != (root.parent / entry['source']).read_bytes():
        raise SystemExit('Checkout differs from snapshot: ' + name)
print('PASS: vendor snapshot' + (' matches checkout' if args.checkout else '')
      + ' (recorded commit ' + manifest['checkoutCommit'][:7] + ')')
