#!/usr/bin/env python3
from __future__ import annotations
import hashlib, json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'Tests' / 'Fixtures' / 'fixture-manifest.json'

def main() -> int:
    try:
        doc = json.loads(MANIFEST.read_text(encoding='utf-8'))
    except Exception as exc:
        print(f'ERROR: cannot read fixture manifest: {exc}', file=sys.stderr)
        return 1
    entries = doc.get('fixtures')
    if not isinstance(entries, list) or not entries:
        print('ERROR: fixture manifest has no fixtures', file=sys.stderr)
        return 1
    seen = set()
    for item in entries:
        path = item.get('path')
        expected = item.get('sha256')
        provenance = item.get('provenance')
        if not isinstance(path, str) or not path.startswith('Tests/Fixtures/'):
            print(f'ERROR: invalid fixture path: {path!r}', file=sys.stderr); return 1
        if path in seen:
            print(f'ERROR: duplicate fixture path: {path}', file=sys.stderr); return 1
        seen.add(path)
        if not isinstance(expected, str) or len(expected) != 64:
            print(f'ERROR: invalid sha256 for {path}', file=sys.stderr); return 1
        if not isinstance(provenance, str) or not provenance.strip():
            print(f'ERROR: missing provenance for {path}', file=sys.stderr); return 1
        p = ROOT / path
        if not p.is_file():
            print(f'ERROR: fixture missing: {path}', file=sys.stderr); return 1
        actual = hashlib.sha256(p.read_bytes()).hexdigest()
        if actual != expected:
            print(f'ERROR: fixture hash mismatch: {path}', file=sys.stderr); return 1
    actual_files = {
        str(p.relative_to(ROOT)).replace('\\', '/')
        for p in (ROOT / 'Tests' / 'Fixtures').iterdir()
        if p.is_file() and p.name != 'fixture-manifest.json' and (p.suffix in {'.bin', '.json'})
    }
    if actual_files != seen:
        missing = sorted(actual_files - seen)
        stale = sorted(seen - actual_files)
        print(f'ERROR: manifest coverage mismatch missing={missing} stale={stale}', file=sys.stderr)
        return 1
    print(f'Fixture manifest: PASS ({len(entries)} files)')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
