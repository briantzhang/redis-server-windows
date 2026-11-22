#!/usr/bin/env python3
"""Generate winget/Scoop/Chocolatey templates for a packaged release."""
from pathlib import Path
import argparse
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', default=str(ROOT/'dist'/'channels'))
    args = parser.parse_args()
    dist = ROOT/'dist'
    archive = dist/('Redis-x64-%s.zip' % args.version)
    if not archive.is_file():
        raise SystemExit('missing release archive: %s' % archive)
    digest = sha256(archive)
    url = ('https://github.com/briantzhang/redis-server-windows/releases/'
           'download/v%s/%s' % (args.version, archive.name))
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    winget = out/('briantzhang.RedisServerWindows.%s.yaml' % args.version)
    winget.write_text('\n'.join([
        'PackageIdentifier: briantzhang.RedisServerWindows',
        'PackageVersion: %s' % args.version,
        'PackageLocale: en-US',
        'Publisher: briantzhang',
        'PackageName: Redis Server for Windows',
        'License: BSD-3-Clause',
        'ShortDescription: Self-maintained Windows x64 Redis backport',
        'InstallerType: zip',
        'Installers:',
        '  - Architecture: x64',
        '    Url: %s' % url,
        '    Sha256: %s' % digest,
        'ManifestType: singleton',
        'ManifestVersion: 1.6.0',
        ''
    ]), encoding='utf-8')
    scoop = out/('redis-server-windows.%s.json' % args.version)
    scoop.write_text(json.dumps({
        'version': args.version,
        'architecture': {'64bit': {'url': url, 'hash': digest}},
        'bin': ['redis-server.exe', 'redis-cli.exe', 'redis-benchmark.exe'],
        'checkver': 'github: briantzhang/redis-server-windows'
    }, indent=2)+'\n', encoding='utf-8')
    choco = out/('redis-server-windows.%s.nuspec.template' % args.version)
    choco.write_text('''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://schemas.microsoft.com/packaging/2015/06/nuspec.xsd">
  <metadata>
    <id>redis-server-windows</id>
    <version>%s</version>
    <title>Redis Server for Windows</title>
    <authors>briantzhang</authors>
    <licenseUrl>https://github.com/briantzhang/redis-server-windows/blob/main/COPYING</licenseUrl>
    <projectUrl>https://github.com/briantzhang/redis-server-windows</projectUrl>
    <description>Self-maintained Windows x64 Redis backport.</description>
    <tags>redis windows x64</tags>
  </metadata>
  <files><file src="tools\\**" target="tools" /></files>
</package>
''' % args.version, encoding='utf-8')
    for path in [winget, scoop, choco, dist/(archive.name + '.sha256')]:
        if path.name.endswith('.sha256'):
            path.write_text('%s  %s\n' % (digest, archive.name), encoding='ascii')
    print(winget)
    print(scoop)
    print(choco)
    print(digest)


if __name__ == '__main__':
    main()
