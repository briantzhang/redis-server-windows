#!/usr/bin/env python3
"""Verify a directory unpacked from a Windows release archive.

The release checksums.sha256 file uses GNU format (two spaces between the
digest and the relative path). It contains its own name; that one line is
skipped so a fresh unpack can always be verified.
"""
import hashlib
import sys
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: verify-checksums.py <unpacked-directory>')
    root = Path(sys.argv[1]).resolve()
    checksums = root / 'checksums.sha256'
    if not checksums.is_file():
        raise SystemExit('missing checksums.sha256: ' + str(checksums))
    failures = []
    checked = set()
    for line in checksums.read_text(encoding='utf-8').splitlines():
        if not line.strip():
            continue
        expected, name = line.split('  ', 1)
        if name == 'checksums.sha256':
            continue
        path = root / name
        checked.add(name)
        if not path.is_file():
            failures.append('MISSING ' + name)
            continue
        actual = digest(path)
        if actual.lower() != expected.lower():
            failures.append('MISMATCH ' + name)
    for path in sorted(root.rglob('*')):
        if not path.is_file() or path.name == 'checksums.sha256':
            continue
        name = path.relative_to(root).as_posix()
        if name not in checked:
            failures.append('EXTRA ' + name)
    if failures:
        for failure in failures:
            print(failure)
        print('FAILED')
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
