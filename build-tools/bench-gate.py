#!/usr/bin/env python3
"""Compare two benchmark JSON reports and gate on configured regressions."""
from pathlib import Path
import argparse
import json
import statistics


def load(path):
    return {row['name']: row for row in json.loads(Path(path).read_text(encoding='utf-8')) if row.get('name')}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--current', required=True)
    parser.add_argument('--max-regression', type=float, default=20.0,
                        help='Maximum allowed median latency increase in percent')
    parser.add_argument('--min-samples', type=int, default=1)
    args = parser.parse_args()
    before, after = load(args.baseline), load(args.current)
    missing = sorted(set(before) - set(after))
    if missing:
        print('Missing benchmarks: ' + ', '.join(missing))
        return 1
    failures = []
    for name, old in before.items():
        new = after[name]
        oldops = float(old.get('logical_operations_per_second', 0))
        newops = float(new.get('logical_operations_per_second', 0))
        if oldops <= 0 or newops <= 0:
            failures.append('%s: invalid throughput' % name)
            continue
        drop = (oldops - newops) / oldops * 100
        if drop > args.max_regression:
            failures.append('%s: %.1f%% throughput drop' % (name, drop))
    if failures:
        for line in failures:
            print(line)
        print('GATE FAILED')
        return 1
    print('GATE OK')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
