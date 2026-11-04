#!/usr/bin/env python3
import runpy, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
helpers = runpy.run_path(str(root / 'tests' / 'backport_regression.py'))
server_path = root / 'msvs' / 'x64' / 'Release' / 'redis-server.exe'
with tempfile.TemporaryDirectory(prefix='redis-aof-repro-') as directory:
    server = helpers['ManagedServer'](str(server_path), directory, 5, 15, aof=True)
    try:
        server.start()
        with helpers['RESP2']('127.0.0.1', server.port, 5) as client:
            try:
                print('SET', client.command('SET', 'k', 'v', 'PX', 180000), flush=True)
            except Exception as exc:
                print(type(exc).__name__, exc, flush=True)
        text = server.log_excerpt()
        start = text.find('REDIS BUG REPORT START')
        print(text[start:start+4000] if start >= 0 else text[-4000:], flush=True)
        print('DIR', directory, flush=True)
    finally:
        server.close()
