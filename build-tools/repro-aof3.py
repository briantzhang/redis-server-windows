#!/usr/bin/env python3
import runpy
from pathlib import Path
root = Path(__file__).resolve().parents[1]
helpers = runpy.run_path(str(root / 'tests' / 'backport_regression.py'))
server_path = root / 'msvs' / 'x64' / 'Release' / 'redis-server.exe'
directory = root / 'build-tools' / 'aof-repro'
directory.mkdir(exist_ok=True)
server = helpers['ManagedServer'](str(server_path), str(directory), 5, 15, aof=True)
try:
    server.start()
    with helpers['RESP2']('127.0.0.1', server.port, 5) as client:
        try:
            print('SET', client.command('SET', 'k', 'v', 'PX', 180000), flush=True)
        except Exception as exc:
            print(type(exc).__name__, exc, flush=True)
    text = (directory / 'server.log').read_text(encoding='utf-8', errors='replace')
    i = text.find('REDIS BUG REPORT START')
    print(text[max(0, i):i+3000], flush=True)
finally:
    server.close()
