#!/usr/bin/env python3
"""Reproduce the AOF crash with one SET PX against a throwaway server."""
import runpy
import tempfile
from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'tests' / 'backport_regression.py'))
server_path = Path(__file__).resolve().parents[1] / 'msvs' / 'x64' / 'Release' / 'redis-server.exe'
with tempfile.TemporaryDirectory(prefix='redis-aof-repro-') as directory:
    server = helpers['ManagedServer'](str(server_path), directory, 5, 15, aof=True)
    try:
        server.start()
        with helpers['RESP2']('127.0.0.1', server.port, 5) as client:
            print('PING', client.command('PING'), flush=True)
            try:
                print('SET', client.command('SET', 'k', 'v', 'PX', 180000), flush=True)
            except Exception as exc:
                print('SET raised', type(exc).__name__, exc, flush=True)
            print('alive', server.process.poll() is None, 'code', server.process.poll(), flush=True)
        print('--- log ---', flush=True)
        print(server.log_excerpt(), flush=True)
    finally:
        server.close()
