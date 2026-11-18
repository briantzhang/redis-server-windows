#!/usr/bin/env python3
"""Measure the hot command paths of a redis-server build over loopback.

Standard library only. The script starts its own server in a temporary
directory (no persistence, no AOF) and drives it through a small RESP2 client.
Every benchmark is a pipeline of `--batch` commands repeated `--iterations`
times; the reported value is the median of `--rounds` rounds, so a single slow
round does not dominate.

    python -I build-tools/bench-hotpaths.py --server msvs/x64/Release/redis-server.exe \
        --label bp44 --output build-tools/bench-bp44-after.json
"""

import argparse
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import sys
import tempfile
import time


class Client:
    def __init__(self, port, password=None, user=None):
        self.sock = socket.create_connection(("127.0.0.1", port), 30)
        self.sock.settimeout(30)
        self.reader = self.sock.makefile("rb")
        if password is not None:
            args = ("AUTH", user, password) if user else ("AUTH", password)
            self.command(*args)

    def close(self):
        try:
            self.reader.close()
        finally:
            self.sock.close()

    @staticmethod
    def encode(args):
        parts = [b"*%d\r\n" % len(args)]
        for arg in args:
            if isinstance(arg, bytes):
                value = arg
            elif isinstance(arg, str):
                value = arg.encode("utf-8")
            elif isinstance(arg, (int, float)):
                value = ("%s" % arg).encode("ascii")
            else:
                raise TypeError("unsupported argument %r" % (arg,))
            parts.extend((b"$%d\r\n" % len(value), value, b"\r\n"))
        return b"".join(parts)

    def read_reply(self):
        line = self.reader.readline()
        if not line.endswith(b"\r\n"):
            raise ValueError("truncated reply %r" % line)
        kind, body = line[:1], line[1:-2]
        if kind in (b"+", b"-", b":"):
            return body
        if kind == b"$":
            length = int(body)
            if length < 0:
                return None
            data = self.reader.read(length + 2)
            return data[:-2]
        if kind == b"*":
            count = int(body)
            if count < 0:
                return None
            return [self.read_reply() for _ in range(count)]
        raise ValueError("unknown reply kind %r" % kind)

    def command(self, *args):
        self.sock.sendall(self.encode(args))
        return self.read_reply()

    def pipeline(self, commands):
        self.sock.sendall(b"".join(self.encode(args) for args in commands))
        for _ in commands:
            self.read_reply()


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_server(executable, directory):
    port = free_port()
    config = Path(directory) / "redis.conf"
    config.write_text(
        "bind 127.0.0.1\nprotected-mode yes\nport %d\n"
        "daemonize no\nsave \"\"\ndir .\ndbfilename dump.rdb\n"
        "logfile server.log\nloglevel warning\n"
        "appendonly no\nappendfsync everysec\n"
        "maxmemory 0\nmaxmemory-policy noeviction\n" % port,
        encoding="ascii")
    log = open(Path(directory) / "stdout.log", "wb")
    process = subprocess.Popen(
        [executable, str(config)], cwd=directory, stdout=log, stderr=log)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("server exited during startup, see %s" % directory)
        try:
            client = Client(port)
            client.command("PING")
            client.close()
            return process, port, log
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start, see %s" % directory)


def build_benchmarks(prefix, restore_payload=None):
    """Return (name, setup_commands, commands) tuples.

    Every action list uses a rotating key so a benchmark never measures the
    same key twice in a row, unless the command itself is key-independent.
    """
    key = lambda index: "%s:%d" % (prefix, index % 512)
    zkey = lambda index: "%sz:%d" % (prefix, index % 64)
    skey = lambda index: "%ss:%d" % (prefix, index % 64)
    hkey = lambda index: "%sh:%d" % (prefix, index % 64)
    xkey = lambda index: "%sx" % prefix
    rkey = lambda index: "%sr:%d" % (prefix, index % 512)

    benchmarks = [
        ("ping_pipeline", [], [("PING",)]),
        ("set_get_sequential", [],
         [("SET", key(0), "value"),
          ("GET", key(0))]),
        ("get_missing", [], [("GET", "missing:key")]),
        ("mget_10", [], [("MGET",) + tuple(key(i) for i in range(10))]),
        ("hash_hset_hget", [],
         [("HSET", hkey(0), "field", "value"),
          ("HGET", hkey(0), "field")]),
        ("sadd_smismember", [],
         [("SADD", skey(0), "member"),
          ("SMISMEMBER", skey(0), "member", "absent")]),
        ("zadd_zmscore", [],
         [("ZADD", zkey(0), 1.5, "member"),
          ("ZMSCORE", zkey(0), "member", "absent")]),
        ("lpush_lrange", [],
         [("LPUSH", key(0), "item"),
          ("LRANGE", key(0), 0, 31)]),
        ("xadd", [], [("XADD", xkey(0), "MAXLEN", 1000, "*", "f", "v")]),
        ("eval_get", [], [("EVAL", "return redis.call('GET', KEYS[1])", 1,
                           key(0))]),
        ("info_server", [], [("INFO", "server")]),
        ("config_get", [], [("CONFIG", "GET", "maxmemory")]),
        ("dbsize", [], [("DBSIZE",)]),
    ]
    if restore_payload is not None:
        benchmarks.extend([
            ("dump_list", [], [("DUMP", rkey(0))]),
            ("restore_list", [],
             [("RESTORE", rkey(0), 0, restore_payload, "REPLACE")]),
        ])
    return benchmarks


class Session:
    """One server plus its client, benchmark definitions and samples."""

    def __init__(self, executable, label, sanitize=None):
        self.label = label
        self.temp = tempfile.TemporaryDirectory(prefix="redis-bench-")
        self.process, self.port, self.log = start_server(
            str(Path(executable).resolve()), self.temp.name)
        self.client = Client(self.port)
        if sanitize is not None:
            reply = self.client.command("CONFIG", "SET",
                                        "sanitize-dump-payload", sanitize)
            if reply != b"OK":
                raise RuntimeError("sanitize-dump-payload is not supported: %r"
                                   % (reply,))
        source = "bench:restore:source"
        self.client.command("DEL", source)
        self.client.command("RPUSH", *([source] + ["value-%d" % index
                                                   for index in range(200)]))
        payload = self.client.command("DUMP", source)
        self.client.command("DEL", source)
        self.benches = build_benchmarks("bench:", payload)
        for _, setup, _ in self.benches:
            for command in setup:
                self.client.command(*command)
        # Warm the allocator and the command cache before measuring.
        for _, _, commands in self.benches:
            self.client.pipeline((commands * 4)[:128])
        self.samples = {name: [] for name, _, _ in self.benches}

    def round(self, iterations, batch):
        for name, _, commands in self.benches:
            cycles = (commands * ((batch + len(commands) - 1)
                                  // len(commands)))[:batch]
            started = time.perf_counter()
            for _ in range(iterations):
                self.client.pipeline(cycles)
            elapsed = time.perf_counter() - started
            self.samples[name].append(elapsed * 1000.0 / iterations)

    def report(self, args):
        info = self.client.command("INFO", "server").decode("utf-8", "replace")
        version = ""
        for line in info.splitlines():
            if line.startswith("redis_version:"):
                version = line.split(":", 1)[1].strip()
        measurements = []
        for name, _, _ in self.benches:
            median = statistics.median(self.samples[name])
            best = min(self.samples[name])
            measurements.append({
                "name": name,
                "median_ms_per_batch": median,
                "logical_operations_per_second": args.batch / (median / 1000.0),
                "best_ms_per_batch": best,
                "best_operations_per_second": args.batch / (best / 1000.0),
            })
        return {
            "label": self.label,
            "scope": "loopback, no AOF, single client, %d rounds; the best round "
                     "filters out load from other processes on the machine"
                     % args.rounds,
            "iterations": args.iterations,
            "batch": args.batch,
            "redis_version": version,
            "measurements": measurements,
        }

    def close(self):
        try:
            self.client.close()
        finally:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
            self.log.close()
            self.temp.cleanup()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--label", default="current")
    parser.add_argument("--server2",
                        help="optional second build measured alternately")
    parser.add_argument("--label2", default="baseline")
    parser.add_argument("--output")
    parser.add_argument("--iterations", type=int, default=200)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--sanitize", choices=("no", "yes", "clients"),
                        help="set sanitize-dump-payload before measuring")
    args = parser.parse_args()

    sessions = [Session(args.server, args.label, args.sanitize)]
    try:
        if args.server2:
            sessions.append(Session(args.server2, args.label2, args.sanitize))
        # Alternate the order every round so slow drift is shared equally.
        for round_index in range(args.rounds):
            order = sessions if round_index % 2 == 0 else list(reversed(sessions))
            for session in order:
                session.round(args.iterations, args.batch)
        reports = [session.report(args) for session in sessions]
    finally:
        for session in sessions:
            session.close()

    if len(reports) == 1:
        payload = reports[0]
    else:
        first = {m["name"]: m for m in reports[0]["measurements"]}
        comparison = []
        for second in reports[1]["measurements"]:
            name = second["name"]
            comparison.append({
                "name": name,
                "first_best_ops_per_second":
                    first[name]["best_operations_per_second"],
                "second_best_ops_per_second":
                    second["best_operations_per_second"],
                "second_vs_first_best_percent":
                    (second["best_operations_per_second"] /
                     first[name]["best_operations_per_second"] - 1) * 100.0,
                "first_median_ops_per_second":
                    first[name]["logical_operations_per_second"],
                "second_median_ops_per_second":
                    second["logical_operations_per_second"],
            })
        payload = {"first": reports[0], "second": reports[1],
                   "comparison": comparison}

    text = json.dumps(payload, indent=1)
    if args.output:
        Path(args.output).write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
