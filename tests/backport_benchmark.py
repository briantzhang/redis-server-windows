#!/usr/bin/env python3
"""Small loopback batch/round-trip benchmark; no third-party dependencies."""
import argparse
import json
from pathlib import Path
import runpy
import statistics
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--iterations', type=int, default=200)
    parser.add_argument('--batch', type=int, default=32)
    parser.add_argument('--output')
    args = parser.parse_args()
    if args.iterations < 1 or args.batch < 1:
        parser.error('iterations and batch must be positive')
    helpers = runpy.run_path(str(Path(__file__).with_name('backport_regression.py')))
    results = {'scope': 'loopback, no AOF, single client, median of 3 runs',
               'iterations': args.iterations, 'batch': args.batch, 'measurements': []}
    with tempfile.TemporaryDirectory(prefix='redis-backport-bench-') as directory:
        server = helpers['ManagedServer'](str(Path(args.server).resolve()), directory, 5, 20)
        try:
            server.start()
            with helpers['RESP2']('127.0.0.1', server.port, 5) as client:
                members = [str(i).encode() for i in range(args.batch)]
                client.command('SADD', 'bench:set', *members)
                zargs = [item for i in range(args.batch) for item in (i, members[i])]
                client.command('ZADD', 'bench:zset', *zargs)
                client.command('SET', 'bench:key', 'v')
                results['server_info'] = client.command('INFO', 'server').decode()

                def seq(commands):
                    for command in commands:
                        client.command(*command)

                sm = [('SISMEMBER', 'bench:set', member) for member in members]
                zm = [('ZSCORE', 'bench:zset', member) for member in members]
                cases = [
                    ('set_members_sequential', args.batch, lambda: seq(sm)),
                    ('set_members_pipeline', args.batch, lambda: client.pipeline(sm)),
                    ('SMISMEMBER', args.batch, lambda: client.command('SMISMEMBER', 'bench:set', *members)),
                    ('zset_scores_sequential', args.batch, lambda: seq(zm)),
                    ('zset_scores_pipeline', args.batch, lambda: client.pipeline(zm)),
                    ('ZMSCORE', args.batch, lambda: client.command('ZMSCORE', 'bench:zset', *members)),
                    ('get_then_set_nonatomic', 1, lambda: seq([('GET', 'bench:key'), ('SET', 'bench:key', 'v')])),
                    ('SET_GET_atomic', 1, lambda: client.command('SET', 'bench:key', 'v', 'GET')),
                ]
                for name, operations, action in cases:
                    for _ in range(5):
                        action()
                    durations = []
                    for _ in range(3):
                        started = time.perf_counter()
                        for _ in range(args.iterations):
                            action()
                        durations.append(time.perf_counter()-started)
                    elapsed = statistics.median(durations)
                    row = {'name': name, 'median_ms_per_batch': elapsed*1000/args.iterations,
                           'logical_operations_per_second': args.iterations*operations/elapsed}
                    results['measurements'].append(row)
                    print(json.dumps(row), flush=True)
        finally:
            server.close()
    encoded = json.dumps(results, indent=2, ensure_ascii=False)
    if args.output:
        Path(args.output).write_text(encoded+'\n', encoding='utf-8')
    else:
        print(encoded)


if __name__ == '__main__':
    main()
