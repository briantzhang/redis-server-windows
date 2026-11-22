#!/usr/bin/env python3
"""Collect TLS benchmark samples through redis-benchmark --tls."""
from pathlib import Path
import argparse
import json
import re
import subprocess
import statistics

ROOT = Path(__file__).resolve().parents[1]


def bench(bench_exe, port, insecure=True, cacert=None, sni=None):
    args = [str(bench_exe), '--tls', '-h', '127.0.0.1', '-p', str(port),
            '-c', '4', '-n', '200', '-q']
    if insecure:
        args.append('--insecure')
    if cacert:
        args += ['--cacert', str(cacert)]
    if sni:
        args += ['--sni', sni]
    result = subprocess.run(args, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=60)
    text = result.stdout.decode('utf-8', 'replace')
    if result.returncode != 0:
        raise SystemExit('benchmark failed: ' + text)
    match = re.search(r'([\d.]+)\s+requests per second', text)
    if not match:
        raise SystemExit('no throughput in benchmark output: ' + text)
    return float(match.group(1))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True, help='Path to redis-server.exe')
    parser.add_argument('--output', default=str(ROOT/'dist'/'benchmark-tls.json'))
    parser.add_argument('--runs', type=int, default=3)
    args = parser.parse_args()
    server = Path(args.server).resolve()
    bench_exe = server.with_name('redis-benchmark.exe')
    if not server.is_file() or not bench_exe.is_file():
        raise SystemExit('server and redis-benchmark.exe must exist')
    # The caller must start a TLS listener; this script measures that endpoint.
    parser.add_argument('--port', type=int, default=6379)
    parser.add_argument('--insecure', action='store_true', default=True)
    opts = parser.parse_args()
    samples = [bench(bench_exe, opts.port, opts.insecure) for _ in range(opts.runs)]
    row = {'name': 'TLS PING', 'runs': samples,
           'median_requests_per_second': statistics.median(samples)}
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps([row], indent=2)+'\n', encoding='utf-8')
    print(json.dumps(row))


if __name__ == '__main__':
    main()
