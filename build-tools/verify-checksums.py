#!/usr/bin/env python3
"""Verify a directory unpacked from a Windows release archive.

The release checksums.sha256 file uses GNU format (two spaces between the
digest and the relative path). It contains its own name; that one line is
skipped so a fresh unpack can always be verified.
"""
import hashlib
import sys
import tempfile
import zipfile
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def verify_root(root):
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


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: verify-checksums.py <unpacked-directory|release.zip>')
    target = Path(sys.argv[1]).resolve()
    if target.is_file():
        if zipfile.is_zipfile(target):
            with tempfile.TemporaryDirectory(prefix='redis-verify-') as temp:
                root = Path(temp)
                with zipfile.ZipFile(target) as archive:
                    names = archive.namelist()
                    prefix = ''
                    if names and all(name.startswith(names[0].split('/')[0] + '/')
                                     for name in names):
                        prefix = names[0].split('/')[0] + '/'
                    if prefix + 'checksums.sha256' not in names:
                        raise SystemExit('release zip has no checksums.sha256')
                    archive.extractall(root)
                    for path in sorted(root.rglob('*'), reverse=True):
                        if path.is_file() and path.name == 'checksums.sha256':
                            for candidate in [path.parent, path.parent.parent]:
                                if (candidate / 'checksums.sha256').is_file():
                                    return verify_root(candidate)
                    raise SystemExit('release zip has no unpackable release directory')
        raise SystemExit('file is not a release zip: ' + str(target))
    if not target.is_dir():
        raise SystemExit('path not found: ' + str(target))
    return verify_root(target)


if __name__ == '__main__':
    raise SystemExit(main())
