#!/usr/bin/env python3
"""Standard-library RESP2 regression tests for the Redis 5 command backport.

Managed instances (one loopback port and temporary directory per test):
    python -I tests/backport_regression.py --server /path/to/redis-server
    python -I tests/backport_regression.py --server D:\\redis\\redis-server.exe

Existing instance (unique key namespace; no FLUSH*, CONFIG, SAVE or SHUTDOWN):
    python -I tests/backport_regression.py --host 127.0.0.1 --port 6379

Select tests with --match (repeatable substring), list with --list, or stop on
first failure with --failfast. Persistence tests require --server and otherwise
report SKIP. --password authenticates connections to an existing instance.
No redis-py, compiler, shell, repository imports, or generated files are needed.

All values and RESP2 bulk replies are bytes; Redis errors are exceptions unless
returned inside EXEC. Tests deliberately check observable state after errors,
not exact error sentences. Each managed server runs in the foreground and only
its own subprocess handle is terminated. Temporary files and connections are
cleaned on success, failure, and normal interruption. External-mode isolation is
by key prefix: it cannot provide its own server directory or random server port.
"""

import argparse
import atexit
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import fnmatch
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid


LLONG_MAX = (1 << 63) - 1
LLONG_MIN = -(1 << 63)
ACTIVE_SERVERS = set()
OPTIONS = None


class RedisError(Exception):
    """An error reply from Redis (including errors nested in EXEC arrays)."""


class RESP2:
    """Small binary-safe RESP2 client; one instance per connection/thread."""

    def __init__(self, host, port, timeout=5.0, password=None):
        self.sock = socket.create_connection((host, port), timeout)
        self.sock.settimeout(timeout)
        self.reader = self.sock.makefile("rb")
        try:
            if password is not None:
                self.command("AUTH", password)
        except BaseException:
            self.close()
            raise

    def close(self):
        try:
            self.reader.close()
        finally:
            self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @staticmethod
    def encode(args):
        parts = [b"*%d\r\n" % len(args)]
        for arg in args:
            if isinstance(arg, bytes):
                value = arg
            elif isinstance(arg, str):
                value = arg.encode("utf-8")
            elif isinstance(arg, int):
                value = str(arg).encode("ascii")
            else:
                raise TypeError("RESP argument must be bytes, str, or int")
            parts.extend((b"$%d\r\n" % len(value), value, b"\r\n"))
        return b"".join(parts)

    def _read(self):
        line = self.reader.readline()
        if not line:
            raise EOFError("Redis closed the connection")
        if not line.endswith(b"\r\n"):
            raise ValueError("Truncated RESP line: %r" % line)
        kind, value = line[:1], line[1:-2]
        if kind == b"+":
            return value
        if kind == b"-":
            return RedisError(value.decode("utf-8", "replace"))
        if kind == b":":
            return int(value)
        if kind in (b"$", b"*"):
            length = int(value)
            if length == -1:
                return None
            if length < 0:
                raise ValueError("Invalid RESP length")
            if kind == b"*":
                return [self._read() for _ in range(length)]
            data = self.reader.read(length)
            if len(data) != length or self.reader.read(2) != b"\r\n":
                raise EOFError("Truncated RESP bulk reply")
            return data
        raise ValueError("Unknown RESP2 type: %r" % kind)

    def command(self, *args):
        self.sock.sendall(self.encode(args))
        result = self._read()
        if isinstance(result, RedisError):
            raise result
        return result

    def pipeline(self, commands):
        """Drain every reply, preserving errors as values so framing stays valid."""
        commands = list(commands)
        self.sock.sendall(b"".join(self.encode(args) for args in commands))
        return [self._read() for _ in commands]


def reserve_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class ManagedServer:
    def __init__(self, executable, directory, timeout, startup_timeout, aof=False):
        self.executable = executable
        self.directory = Path(directory)
        self.timeout = timeout
        self.startup_timeout = startup_timeout
        self.aof = aof
        self.port = reserve_port()
        self.process = None
        self.output = None
        self.ready = False
        ACTIVE_SERVERS.add(self)

    def log_excerpt(self):
        excerpts = []
        for name in ("stdout.log", "server.log"):
            path = self.directory / name
            try:
                with path.open("rb") as stream:
                    stream.seek(max(0, path.stat().st_size - 6000))
                    excerpts.append(name + ":\n" + stream.read().decode("utf-8", "replace"))
            except OSError:
                pass
        return "\n".join(excerpts)

    def start(self):
        # Port reservation cannot be transferred to Redis. Never trust PING alone:
        # verify INFO's PID before considering this port to be our child server.
        config = self.directory / "redis.conf"
        config.write_text(
            "bind 127.0.0.1\nprotected-mode yes\nport %d\n"
            "daemonize no\nsave \"\"\ndir .\ndbfilename dump.rdb\n"
            "logfile server.log\nloglevel notice\n"
            "appendonly %s\nappendfilename appendonly.aof\n"
            "appendfsync always\naof-use-rdb-preamble no\n"
            "auto-aof-rewrite-percentage 0\n" % (self.port, "yes" if self.aof else "no"),
            encoding="ascii",
        )
        self.output = (self.directory / "stdout.log").open("ab", buffering=0)
        try:
            self.process = subprocess.Popen(
                [self.executable, str(config)], cwd=str(self.directory),
                stdin=subprocess.DEVNULL, stdout=self.output, stderr=subprocess.STDOUT,
            )
            end = time.monotonic() + self.startup_timeout
            last_error = None
            while time.monotonic() < end:
                if self.process.poll() is not None:
                    break
                try:
                    with RESP2("127.0.0.1", self.port, min(self.timeout, 0.5)) as client:
                        info = client.command("INFO", "server")
                        pid = next((line.split(b":", 1)[1] for line in info.splitlines()
                                    if line.startswith(b"process_id:")), None)
                        if pid == str(self.process.pid).encode("ascii"):
                            if client.command("PING") == b"PONG" and self.process.poll() is None:
                                self.ready = True
                                return
                        else:
                            last_error = RuntimeError("Port belongs to another process; refusing to use it")
                except (OSError, EOFError, RedisError, ValueError) as exc:
                    last_error = exc
                time.sleep(0.03)
            raise RuntimeError("Managed Redis did not start: %s\n%s" %
                               (last_error, self.log_excerpt()))
        except BaseException:
            self.stop()
            raise

    def stop(self):
        process = self.process
        self.ready = False
        try:
            if process is not None and process.poll() is None:
                # Never send SHUTDOWN to a port that might have been reused.
                # terminate/kill target only the child process we actually own.
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        finally:
            if self.output is not None:
                self.output.close()
                self.output = None
            if process is None or process.poll() is not None:
                self.process = None

    def close(self):
        try:
            self.stop()
        finally:
            ACTIVE_SERVERS.discard(self)

    def restart(self):
        self.stop()
        self.start()


def cleanup_servers():
    for server in list(ACTIVE_SERVERS):
        server.close()


atexit.register(cleanup_servers)


class BackportRegression(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="redis-backport-")
        self.addCleanup(self.temp.cleanup)
        self.keys = set()
        self.prefix = ("backport:%s:" % uuid.uuid4().hex).encode("ascii")
        self.server = None
        if OPTIONS.server:
            self.server = ManagedServer(
                OPTIONS.server, self.temp.name, OPTIONS.timeout,
                OPTIONS.startup_timeout, aof="aof" in self._testMethodName,
            )
            self.addCleanup(self.server.close)
            self.server.start()
            self.host, self.port, self.password = "127.0.0.1", self.server.port, None
        else:
            self.host, self.port, self.password = OPTIONS.host, OPTIONS.port, OPTIONS.password
        self.addCleanup(self.clean_keys)
        self.client = self.connect()
        self.addCleanup(self.client.close)
        self.assertEqual(self.client.command("PING"), b"PONG")

    def connect(self):
        return RESP2(self.host, self.port, OPTIONS.timeout, self.password)

    def clean_keys(self):
        if self.keys:
            try:
                with self.connect() as client:
                    client.command("DEL", *sorted(self.keys))
            except (OSError, EOFError, RedisError):
                # A managed crash is already reported by the test; still stop it
                # and remove its directory. Never touch keys outside this prefix.
                pass

    def key(self, name):
        value = self.prefix + str(name).encode("utf-8")
        self.keys.add(value)
        return value

    def cmd(self, *args):
        return self.client.command(*args)

    def now_ms(self):
        seconds, micros = self.cmd("TIME")
        return int(seconds) * 1000 + int(micros) // 1000

    def assert_ttl(self, key, low=1000, high=180000):
        ttl = self.cmd("PTTL", key)
        self.assertGreaterEqual(ttl, low, "TTL too small for %r: %r" % (key, ttl))
        self.assertLessEqual(ttl, high, "TTL too large for %r: %r" % (key, ttl))
        return ttl

    def assert_kept_ttl(self, key, before, elapsed):
        after = self.cmd("PTTL", key)
        self.assertGreater(after, 0)
        self.assertLessEqual(after, before + 30)
        self.assertGreaterEqual(after, before - int(elapsed * 1000) - 500)

    def error_unchanged(self, key, args, error=None):
        """Check value AND deadline remain unchanged after a rejected command."""
        before_value = self.cmd("DUMP", key)
        before_ttl = self.cmd("PTTL", key)
        started = time.monotonic()
        with self.assertRaises(RedisError) as caught:
            self.cmd(*args)
        if error:
            self.assertIn(error.lower(), str(caught.exception).lower())
        self.assertEqual(self.cmd("DUMP", key), before_value, repr(args))
        if before_ttl >= 0:
            self.assert_kept_ttl(key, before_ttl, time.monotonic() - started)
        else:
            self.assertEqual(self.cmd("PTTL", key), before_ttl, repr(args))

    def expire_argument(self, command, delay_ms):
        if command == "EXPIRE":
            return delay_ms // 1000
        if command == "PEXPIRE":
            return delay_ms
        if command == "EXPIREAT":
            return (self.now_ms() + delay_ms) // 1000
        return self.now_ms() + delay_ms

    def restart(self):
        if self.server is None:
            self.skipTest("Restart/persistence tests require --server")
        self.client.close()
        self.server.restart()
        self.client = self.connect()
        self.addCleanup(self.client.close)

    def race(self, commands):
        barrier = threading.Barrier(len(commands))

        def run(args):
            with self.connect() as client:
                barrier.wait(timeout=OPTIONS.timeout)
                return client.command(*args)

        with ThreadPoolExecutor(max_workers=len(commands)) as pool:
            futures = [pool.submit(run, args) for args in commands]
            return [future.result(timeout=OPTIONS.timeout * 2) for future in futures]

    def test_old_commands_smoke(self):
        key, other = self.key("string"), self.key("other")
        self.assertEqual(self.cmd("SET", key, "1"), b"OK")
        self.assertEqual(self.cmd("INCR", key), 2)
        self.assertEqual(self.cmd("APPEND", key, "3"), 2)
        self.assertEqual(self.cmd("GETSET", key, "next"), b"23")
        self.assertEqual(self.cmd("SETNX", key, "no"), 0)
        self.assertEqual(self.cmd("MSET", key, "a", other, "b"), b"OK")
        self.assertEqual(self.cmd("MGET", key, other, self.key("missing")), [b"a", b"b", None])
        self.assertEqual(self.cmd("SETEX", key, 120, "s"), b"OK")
        self.assert_ttl(key, 100000, 120000)
        self.assertEqual(self.cmd("PSETEX", key, 120000, "p"), b"OK")
        self.assert_ttl(key, 100000, 120000)
        self.assertEqual(self.cmd("PERSIST", key), 1)
        self.assertEqual(self.cmd("TTL", key), -1)
        self.assertEqual(self.cmd("EXPIRE", key, 120), 1)
        self.assertEqual(self.cmd("PEXPIRE", key, 120000), 1)
        self.assertEqual(self.cmd("EXPIREAT", key, self.now_ms() // 1000 + 120), 1)
        self.assertEqual(self.cmd("PEXPIREAT", key, self.now_ms() + 120000), 1)
        self.assertEqual(self.cmd("DEL", key), 1)
        self.assertEqual(self.cmd("TTL", key), -2)
        listing, hashing = self.key("list"), self.key("hash")
        self.assertEqual(self.cmd("RPUSH", listing, "a", "b"), 2)
        self.assertEqual(self.cmd("LRANGE", listing, 0, -1), [b"a", b"b"])
        self.assertEqual(self.cmd("HSET", hashing, "f", "v"), 1)
        self.assertEqual(self.cmd("HGET", hashing, "f"), b"v")

    def test_zmscore_order_missing_duplicates_and_readonly(self):
        key = self.key("zset")
        self.assertEqual(self.cmd("ZMSCORE", key, "a", "missing", "a"), [None, None, None])
        self.assertEqual(self.cmd("EXISTS", key), 0)
        binary = b"\x00member\r\n\xff"
        self.assertEqual(self.cmd("ZADD", key, "1.5", "a", -2, binary, 0, b"", "+inf", "infinite"), 4)
        self.cmd("PEXPIRE", key, 120000)
        before, started = self.cmd("PTTL", key), time.monotonic()
        expected = [b"-2", None, b"1.5", b"-2", b"0", b"inf"]
        with self.connect() as observer:
            observer.command("WATCH", key)
            self.assertEqual(self.cmd("zMsCoRe", key, binary, "missing", "a", binary, b"", "infinite"), expected)
            observer.command("MULTI")
            observer.command("ZCARD", key)
            self.assertEqual(observer.command("EXEC"), [4])
        self.assert_kept_ttl(key, before, time.monotonic() - started)
        # A long member forces Redis 5's ziplist representation into a skiplist.
        long_member = b"long-" + b"x" * 256
        self.assertEqual(self.cmd("ZADD", key, "3.25", long_member), 1)
        self.assertEqual(self.cmd("ZMSCORE", key, long_member, "a", "missing"), [b"3.25", b"1.5", None])
        self.error_unchanged(key, ("ZMSCORE", key))
        wrong = self.key("string")
        self.cmd("SET", wrong, "v", "PX", 120000)
        self.error_unchanged(wrong, ("ZMSCORE", wrong, "v"), "WRONGTYPE")
        with self.assertRaises(RedisError):
            self.cmd("ZMSCORE")

    def test_smismember_missing_binary_duplicates_and_wrongtype(self):
        key = self.key("set")
        binary = b"\x00member\r\n\xff"
        self.assertEqual(self.cmd("SMISMEMBER", key, "a", "a", "missing"), [0, 0, 0])
        self.assertEqual(self.cmd("EXISTS", key), 0)
        # Exercise both an integer-encoded set and its conversion to a hash set.
        self.assertEqual(self.cmd("SADD", key, 1, 2, 3), 3)
        self.assertEqual(self.cmd("SMISMEMBER", key, 1, 9, 1, "01"), [1, 0, 1, 0])
        self.assertEqual(self.cmd("SADD", key, binary, b""), 2)
        self.cmd("PEXPIRE", key, 120000)
        before, started = self.cmd("PTTL", key), time.monotonic()
        self.assertEqual(self.cmd("SMISMEMBER", key, binary, b"", "missing", binary), [1, 1, 0, 1])
        self.assert_kept_ttl(key, before, time.monotonic() - started)
        self.assertEqual(self.cmd("SCARD", key), 5)
        self.error_unchanged(key, ("SMISMEMBER", key))
        wrong = self.key("string")
        self.cmd("SET", wrong, "v", "PX", 120000)
        self.error_unchanged(wrong, ("SMISMEMBER", wrong, "v"), "WRONGTYPE")
        with self.assertRaises(RedisError):
            self.cmd("SMISMEMBER")

    def test_expiretime_and_pexpiretime(self):
        key = self.key("key")
        for command in ("EXPIRETIME", "PEXPIRETIME"):
            self.assertEqual(self.cmd(command, key), -2)
        self.cmd("RPUSH", key, "a")
        for command in ("EXPIRETIME", "PEXPIRETIME"):
            self.assertEqual(self.cmd(command, key), -1)
        deadline = self.now_ms() + 120123
        self.assertEqual(self.cmd("PEXPIREAT", key, deadline), 1)
        self.assertEqual(self.cmd("PEXPIRETIME", key), deadline)
        self.assertEqual(self.cmd("EXPIRETIME", key), (deadline + 500) // 1000)
        with self.connect() as observer:
            observer.command("WATCH", key)
            self.assertEqual(self.cmd("PEXPIRETIME", key), deadline)
            self.assertEqual(self.cmd("EXPIRETIME", key), (deadline + 500) // 1000)
            observer.command("MULTI")
            observer.command("LLEN", key)
            self.assertEqual(observer.command("EXEC"), [1])
        for command in ("EXPIRETIME", "PEXPIRETIME"):
            self.error_unchanged(key, (command, key, "extra"))
            with self.assertRaises(RedisError):
                self.cmd(command)
        self.assertEqual(self.cmd("PEXPIREAT", key, LLONG_MAX), 1)
        self.assertEqual(self.cmd("PEXPIRETIME", key), LLONG_MAX)
        rounded_max = LLONG_MAX // 1000 + (LLONG_MAX % 1000 + 500) // 1000
        self.assertEqual(self.cmd("EXPIRETIME", key), rounded_max)
        self.assertEqual(self.cmd("PEXPIREAT", key, 1), 1)
        for command in ("EXPIRETIME", "PEXPIRETIME"):
            self.assertEqual(self.cmd(command, key), -2)

    def test_set_get_missing_binary_and_conditions(self):
        key = self.key("key")
        binary = b"\x00old\r\n\xff"
        self.assertIsNone(self.cmd("SET", key, binary, "GET"))
        self.assertEqual(self.cmd("SET", key, b"", "GET"), binary)
        self.assertEqual(self.cmd("SET", key, "xx", "XX", "GET"), b"")
        self.assertEqual(self.cmd("SET", key, "rejected", "NX", "GET"), b"xx")
        self.assertEqual(self.cmd("GET", key), b"xx")
        missing = self.key("missing")
        self.assertIsNone(self.cmd("SET", missing, "no", "XX", "GET"))
        self.assertEqual(self.cmd("EXISTS", missing), 0)
        self.assertIsNone(self.cmd("SET", missing, "yes", "GET", "NX"))
        self.assertEqual(self.cmd("GET", missing), b"yes")
        self.assertIsNone(self.cmd("SET", key, "no", "NX"))
        self.assertEqual(self.cmd("SET", key, "mixed", "gEt", "xX"), b"xx")

    def test_set_keepttl_and_default_clear(self):
        key = self.key("key")
        self.cmd("SET", key, "old", "PX", 120000)
        before, started = self.cmd("PTTL", key), time.monotonic()
        self.assertEqual(self.cmd("SET", key, "new", "KEEPTTL", "GET"), b"old")
        self.assert_kept_ttl(key, before, time.monotonic() - started)
        self.assertEqual(self.cmd("SET", key, "skip", "NX", "GET", "KEEPTTL"), b"new")
        self.assert_kept_ttl(key, before, time.monotonic() - started)
        self.assertEqual(self.cmd("SET", key, "xx", "XX", "KEEPTTL"), b"OK")
        self.assert_kept_ttl(key, before, time.monotonic() - started)
        self.assertEqual(self.cmd("SET", key, "clear", "GET"), b"xx")
        self.assertEqual(self.cmd("PTTL", key), -1)
        persistent = self.key("persistent")
        self.assertEqual(self.cmd("SET", persistent, "v", "KEEPTTL"), b"OK")
        self.assertEqual(self.cmd("PTTL", persistent), -1)
        missing = self.key("xx-missing")
        self.assertIsNone(self.cmd("SET", missing, "v", "XX", "KEEPTTL"))
        self.assertEqual(self.cmd("EXISTS", missing), 0)

    def test_set_absolute_deadline_and_past(self):
        for option in ("EXAT", "PXAT"):
            with self.subTest(option=option):
                key = self.key(option)
                self.cmd("SET", key, "old")
                now = self.now_ms()
                deadline = now + 120000
                argument = deadline // 1000 if option == "EXAT" else deadline
                self.assertEqual(self.cmd("SET", key, "new", option, argument, "GET"), b"old")
                self.assert_ttl(key, 115000, 120000)
                self.assertEqual(self.cmd("SET", key, "gone", "GET", option, 1), b"new")
                self.assertIsNone(self.cmd("GET", key))
                self.assertEqual(self.cmd("PTTL", key), -2)
                self.cmd("SET", key, "retain", "PX", 120000)
                before, started = self.cmd("PTTL", key), time.monotonic()
                self.assertEqual(self.cmd("SET", key, "no", "NX", "GET", option, 1), b"retain")
                self.assert_kept_ttl(key, before, time.monotonic() - started)

    def test_set_wrongtype_and_invalid_atomicity(self):
        key = self.key("list")
        self.cmd("RPUSH", key, "a", "b")
        self.cmd("PEXPIRE", key, 120000)
        for flags in (("GET",), ("NX", "GET"), ("XX", "GET", "KEEPTTL")):
            with self.subTest(flags=flags):
                self.error_unchanged(key, ("SET", key, "v") + flags, "WRONGTYPE")
        # SET without GET is allowed to replace another type, even with KEEPTTL.
        before, started = self.cmd("PTTL", key), time.monotonic()
        self.assertEqual(self.cmd("SET", key, "string", "KEEPTTL"), b"OK")
        self.assert_kept_ttl(key, before, time.monotonic() - started)
        self.cmd("SET", key, "old", "PX", 120000)
        invalid = [(), ("bogus",), ("EXAT",), ("PXAT",), ("NX", "XX"),
                   ("GET", "bogus"), ("KEEPTTL", "EX", 10),
                   ("KEEPTTL", "PX", 10), ("KEEPTTL", "EXAT", 100),
                   ("KEEPTTL", "PXAT", 100), ("EX", 10, "PX", 10),
                   ("EXAT", 100, "PXAT", 100), ("EX", 10, "EXAT", 100),
                   ("PX", 10, "PXAT", 100)]
        for flags in invalid:
            with self.subTest(flags=flags):
                args = ("SET", key) if not flags else ("SET", key, "new") + flags
                self.error_unchanged(key, args)
        for option in ("EX", "PX", "EXAT", "PXAT"):
            for value in (0, -1, "abc", "1.5", "", LLONG_MAX + 1, LLONG_MIN - 1):
                with self.subTest(option=option, value=value):
                    self.error_unchanged(key, ("SET", key, "new", option, value))
                    # Validate options even when NX would reject the write.
                    self.error_unchanged(key, ("SET", key, "new", "NX", option, value))
        for option in ("EX", "PX", "EXAT"):
            values = (LLONG_MAX, LLONG_MAX // 1000 + 1) if option != "PX" else (LLONG_MAX,)
            if option == "EX":
                values += (LLONG_MAX // 1000,)  # Conversion fits; adding 'now' does not.
            for value in values:
                with self.subTest(overflow=option, value=value):
                    self.error_unchanged(key, ("SET", key, "new", option, value))
        far = self.key("far")
        self.assertEqual(self.cmd("SET", far, "v", "PXAT", LLONG_MAX), b"OK")
        self.assertGreater(self.cmd("PTTL", far), 10 ** 15)

    def test_getdel_missing_wrongtype_and_binary(self):
        key = self.key("key")
        self.assertIsNone(self.cmd("GETDEL", key))
        value = b"\x00\xff\r\nvalue"
        self.cmd("SET", key, value, "PX", 120000)
        self.assertEqual(self.cmd("GETDEL", key), value)
        self.assertIsNone(self.cmd("GETDEL", key))
        self.assertEqual(self.cmd("PTTL", key), -2)
        self.cmd("RPUSH", key, "a")
        self.cmd("PEXPIRE", key, 120000)
        self.error_unchanged(key, ("GETDEL", key), "WRONGTYPE")
        self.error_unchanged(key, ("GETDEL", key, "extra"))
        with self.assertRaises(RedisError):
            self.cmd("GETDEL")

    def test_getex_all_modes_and_persist(self):
        key = self.key("key")
        self.assertIsNone(self.cmd("GETEX", key))
        for option in ("EX", "PX", "EXAT", "PXAT", "PERSIST"):
            arg = () if option == "PERSIST" else (120 if option == "EX" else
                  120000 if option == "PX" else self.now_ms() // 1000 + 120
                  if option == "EXAT" else self.now_ms() + 120000,)
            self.assertIsNone(self.cmd("GETEX", key, option, *arg))
            self.assertEqual(self.cmd("EXISTS", key), 0)
        value = b"\x00\r\n\xff"
        self.cmd("SET", key, value, "PX", 120000)
        before, started = self.cmd("PTTL", key), time.monotonic()
        self.assertEqual(self.cmd("GETEX", key), value)
        self.assert_kept_ttl(key, before, time.monotonic() - started)
        for option, arg in (("EX", 120), ("PX", 120000),
                            ("EXAT", self.now_ms() // 1000 + 120),
                            ("PXAT", self.now_ms() + 120000)):
            with self.subTest(option=option):
                self.assertEqual(self.cmd("GETEX", key, option.lower(), arg), value)
                self.assert_ttl(key, 110000, 120000)
                self.assertEqual(self.cmd("GET", key), value)
        self.assertEqual(self.cmd("GETEX", key, "pErSiSt"), value)
        self.assertEqual(self.cmd("PTTL", key), -1)
        self.assertEqual(self.cmd("GETEX", key, "PERSIST"), value)
        self.assertEqual(self.cmd("PTTL", key), -1)
        self.cmd("SET", key, b"")
        self.assertEqual(self.cmd("GETEX", key, "PX", 120000), b"")
        self.assert_ttl(key, 110000, 120000)

    def test_getex_past_deletes_and_relative_expires(self):
        key = self.key("key")
        for option in ("EXAT", "PXAT"):
            self.cmd("SET", key, "old", "PX", 120000)
            self.assertEqual(self.cmd("GETEX", key, option, 1), b"old")
            self.assertIsNone(self.cmd("GET", key))
            self.assertEqual(self.cmd("PTTL", key), -2)
        # Poll instead of assuming an exact scheduler/sleep boundary.
        self.cmd("SET", key, "short")
        self.assertEqual(self.cmd("GETEX", key, "PX", 40), b"short")
        end = time.monotonic() + 3
        while self.cmd("GET", key) is not None and time.monotonic() < end:
            time.sleep(0.01)
        self.assertIsNone(self.cmd("GETEX", key, "PERSIST"))
        self.assertEqual(self.cmd("PTTL", key), -2)
        self.assertIsNone(self.cmd("SET", key, "renewed", "GET", "KEEPTTL"))
        self.assertEqual(self.cmd("PTTL", key), -1)

    def test_getex_invalid_and_wrongtype_atomicity(self):
        key, missing = self.key("key"), self.key("missing")
        self.cmd("SET", key, "old", "PX", 120000)
        invalid_syntax = [("bogus",), ("EX",), ("PX",), ("EXAT",), ("PXAT",),
                          ("PERSIST", "extra"), ("PERSIST", "EX", 10),
                          ("PX", 100, "PERSIST"), ("EX", 10, "PX", 100),
                          ("EXAT", 100, "PXAT", 100), ("KEEPTTL",)]
        for flags in invalid_syntax:
            with self.subTest(syntax=flags):
                self.error_unchanged(key, ("GETEX", key) + flags)
                self.error_unchanged(missing, ("GETEX", missing) + flags)
        invalid_values = []
        for option in ("EX", "PX", "EXAT", "PXAT"):
            invalid_values.extend((option, value) for value in
                                  (0, -1, "abc", "1.5", "", LLONG_MAX + 1, LLONG_MIN - 1))
        invalid_values.extend((option, LLONG_MAX) for option in ("EX", "PX", "EXAT"))
        invalid_values.extend((option, LLONG_MAX // 1000 + 1) for option in ("EX", "EXAT"))
        invalid_values.append(("EX", LLONG_MAX // 1000))
        for flags in invalid_values:
            with self.subTest(value=flags):
                self.error_unchanged(key, ("GETEX", key) + flags)
                # GETEX parses option syntax, then looks up the key before
                # validating its expiration value. A missing key returns nil.
                self.assertIsNone(self.cmd("GETEX", missing, *flags))
                self.assertEqual(self.cmd("EXISTS", missing), 0)
        with self.assertRaises(RedisError):
            self.cmd("GETEX")
        listing = self.key("list")
        self.cmd("RPUSH", listing, "a")
        self.cmd("PEXPIRE", listing, 120000)
        for flags in ((), ("PERSIST",), ("PX", 120000), ("EXAT", 1)):
            with self.subTest(wrongtype=flags):
                self.error_unchanged(listing, ("GETEX", listing) + flags, "WRONGTYPE")
        self.assertEqual(self.cmd("GETEX", key, "PXAT", LLONG_MAX), b"old")
        self.assertGreater(self.cmd("PTTL", key), 10 ** 15)

    def test_expire_condition_matrix(self):
        for command in ("EXPIRE", "PEXPIRE", "EXPIREAT", "PEXPIREAT"):
            with self.subTest(command=command):
                key = self.key(command)
                later = lambda delay: self.expire_argument(command, delay)
                for condition in ("NX", "XX", "GT", "LT", "XX GT", "XX LT"):
                    self.assertEqual(self.cmd(command, key, later(120000), *condition.split()), 0)
                self.cmd("SET", key, "v")
                self.assertEqual(self.cmd(command, key, later(120000), "XX"), 0)
                self.assertEqual(self.cmd(command, key, later(120000), "GT"), 0)
                self.assertEqual(self.cmd("PTTL", key), -1)
                # A persistent key has an infinite TTL for GT/LT comparisons.
                self.assertEqual(self.cmd(command, key, later(120000), "LT"), 1)
                before, started = self.cmd("PTTL", key), time.monotonic()
                self.assertEqual(self.cmd(command, key, later(180000), "NX"), 0)
                self.assertEqual(self.cmd(command, key, later(180000), "LT"), 0)
                self.assertEqual(self.cmd(command, key, later(60000), "GT"), 0)
                self.assert_kept_ttl(key, before, time.monotonic() - started)
                self.assertEqual(self.cmd(command, key, later(180000), "XX", "GT"), 1)
                self.assert_ttl(key, 170000, 180000)
                self.assertEqual(self.cmd(command, key, later(60000), "LT", "XX"), 1)
                self.assert_ttl(key, 50000, 60000)
                self.assertEqual(self.cmd(command, key, later(120000), "xX"), 1)
                self.assertEqual(self.cmd("PERSIST", key), 1)
                self.assertEqual(self.cmd(command, key, later(120000), "XX", "LT"), 0)
                self.assertEqual(self.cmd(command, key, later(120000), "XX", "GT"), 0)
                self.assertEqual(self.cmd(command, key, later(120000), "nX"), 1)
                self.assert_ttl(key, 110000, 120000)
                self.assertEqual(self.cmd("GET", key), b"v")

    def test_expire_equal_deadline_is_strict(self):
        # Absolute deadlines avoid a moving 'now' changing equality comparisons.
        key = self.key("key")
        for command, deadline in (("EXPIREAT", self.now_ms() // 1000 + 120),
                                  ("PEXPIREAT", self.now_ms() + 120000)):
            self.cmd("SET", key, "v")
            self.assertEqual(self.cmd(command, key, deadline, "NX"), 1)
            for flags in (("GT",), ("LT",), ("XX", "GT"), ("XX", "LT")):
                self.assertEqual(self.cmd(command, key, deadline, *flags), 0)
            self.assertEqual(self.cmd(command, key, deadline, "XX"), 1)

    def test_expire_past_conditions_and_nonstring(self):
        for command in ("EXPIRE", "PEXPIRE", "EXPIREAT", "PEXPIREAT"):
            for past in (0, -1):
                with self.subTest(command=command, past=past):
                    key = self.key("%s:%s" % (command, past))
                    self.cmd("RPUSH", key, "a", "b")
                    self.assertEqual(self.cmd(command, key, past, "XX"), 0)
                    self.assertEqual(self.cmd("LLEN", key), 2)
                    self.assertEqual(self.cmd(command, key, past, "GT"), 0)
                    self.assertEqual(self.cmd(command, key, past, "NX"), 1)
                    self.assertEqual(self.cmd("EXISTS", key), 0)
                    self.cmd("RPUSH", key, "a")
                    self.cmd("PEXPIRE", key, 120000)
                    self.assertEqual(self.cmd(command, key, past, "NX"), 0)
                    self.assertEqual(self.cmd(command, key, past, "GT"), 0)
                    self.assertEqual(self.cmd("LLEN", key), 1)
                    self.assertEqual(self.cmd(command, key, past, "XX", "LT"), 1)
                    self.assertEqual(self.cmd("PTTL", key), -2)

    def test_expire_invalid_combinations_and_overflow(self):
        key, missing = self.key("key"), self.key("missing")
        self.cmd("SET", key, "v", "PX", 120000)
        for command in ("EXPIRE", "PEXPIRE", "EXPIREAT", "PEXPIREAT"):
            invalid_flags = [("NX", "XX"), ("NX", "GT"), ("NX", "LT"),
                             ("GT", "LT"), ("XX", "GT", "LT"), ("bogus",),
                             ("XX", "bogus"), ("NX", "XX", "GT", "LT")]
            for flags in invalid_flags:
                with self.subTest(command=command, flags=flags):
                    value = self.expire_argument(command, 120000)
                    self.error_unchanged(key, (command, key, value) + flags)
                    self.error_unchanged(missing, (command, missing, value) + flags)
            bad_values = ["abc", "1.5", "", LLONG_MAX + 1, LLONG_MIN - 1]
            if command != "PEXPIREAT":
                bad_values.append(LLONG_MAX)
            if command in ("EXPIRE", "EXPIREAT"):
                bad_values.append(LLONG_MIN)
            for value in bad_values:
                with self.subTest(command=command, value=value):
                    self.error_unchanged(key, (command, key, value, "XX"))
                    self.error_unchanged(missing, (command, missing, value))
            self.error_unchanged(key, (command, key))
            with self.assertRaises(RedisError):
                self.cmd(command)
        far = self.key("far")
        self.cmd("SET", far, "v")
        self.assertEqual(self.cmd("PEXPIREAT", far, LLONG_MAX, "NX"), 1)
        self.assertGreater(self.cmd("PTTL", far), 10 ** 15)
        self.assertEqual(self.cmd("PEXPIREAT", far, LLONG_MIN, "LT"), 1)
        self.assertEqual(self.cmd("EXISTS", far), 0)
        self.cmd("SET", far, "v")
        self.assertEqual(self.cmd("PEXPIRE", far, LLONG_MIN), 1)
        self.assertEqual(self.cmd("EXISTS", far), 0)

    def test_transactions_and_pipeline_error_framing(self):
        key, listing = self.key("key"), self.key("list")
        self.cmd("SET", key, "old", "PX", 120000)
        self.cmd("RPUSH", listing, "a")
        replies = self.client.pipeline([
            ("MULTI",), ("SET", key, "next", "GET", "KEEPTTL"),
            ("GETEX", key, "PERSIST"), ("GETDEL", key), ("GET", key), ("EXEC",),
        ])
        self.assertEqual(replies, [b"OK", b"QUEUED", b"QUEUED", b"QUEUED", b"QUEUED",
                                   [b"old", b"next", b"next", None]])
        self.assertEqual(self.cmd("EXISTS", key), 0)
        replies = self.client.pipeline([
            ("MULTI",), ("GETDEL", listing), ("SET", key, "ok", "GET"), ("EXEC",),
            ("GETEX", listing), ("PING",),
        ])
        self.assertIsInstance(replies[3][0], RedisError)
        self.assertIn("WRONGTYPE", str(replies[3][0]))
        self.assertIsNone(replies[3][1])
        self.assertIsInstance(replies[4], RedisError)
        self.assertEqual(replies[5], b"PONG")
        self.assertEqual(self.cmd("LRANGE", listing, 0, -1), [b"a"])
        self.assertEqual(self.cmd("GET", key), b"ok")
        self.error_unchanged(key, ("GETEX", key, "PX", LLONG_MAX))

    def test_watch_observes_only_actual_mutations(self):
        key, marker = self.key("key"), self.key("marker")
        self.cmd("SET", key, "v", "PX", 120000)
        with self.connect() as observer:
            for args in (("GETEX", key), ("SET", key, "no", "NX", "GET"),
                         ("PEXPIRE", key, 180000, "NX"),
                         ("PEXPIRE", key, 60000, "GT")):
                self.assertEqual(observer.command("WATCH", key), b"OK")
                self.cmd(*args)
                observer.command("MULTI")
                observer.command("SET", marker, "ok")
                self.assertEqual(observer.command("EXEC"), [b"OK"], repr(args))
            # Invalid commands likewise must not dirty the watched key.
            observer.command("WATCH", key)
            self.error_unchanged(key, ("GETEX", key, "PX", LLONG_MAX))
            observer.command("MULTI")
            observer.command("GET", key)
            self.assertEqual(observer.command("EXEC"), [b"v"])
            for args in (("SET", key, "v", "GET", "KEEPTTL"),
                         ("GETEX", key, "PX", 120000),
                         ("GETEX", key, "PERSIST"), ("GETDEL", key)):
                observer.command("WATCH", key)
                self.cmd(*args)
                observer.command("MULTI")
                observer.command("GET", key)
                self.assertIsNone(observer.command("EXEC"), repr(args))
            # Missing GETDEL and PERSIST on a persistent key are no-ops.
            observer.command("WATCH", key)
            self.assertIsNone(self.cmd("GETDEL", key))
            observer.command("MULTI")
            observer.command("GET", key)
            self.assertEqual(observer.command("EXEC"), [None])
            self.cmd("SET", key, "persistent")
            observer.command("WATCH", key)
            self.assertEqual(self.cmd("GETEX", key, "PERSIST"), b"persistent")
            observer.command("MULTI")
            observer.command("GET", key)
            self.assertEqual(observer.command("EXEC"), [b"persistent"])

    def test_concurrent_getdel_exactly_one_winner(self):
        key = self.key("key")
        self.cmd("SET", key, "token", "PX", 120000)
        replies = self.race([("GETDEL", key)] * 8)
        self.assertEqual(Counter(replies), Counter({b"token": 1, None: 7}))
        self.assertEqual(self.cmd("EXISTS", key), 0)

    def test_concurrent_set_get_forms_atomic_chain(self):
        key = self.key("key")
        self.cmd("SET", key, "initial", "PX", 120000)
        before, started = self.cmd("PTTL", key), time.monotonic()
        values = [("value-%d" % i).encode("ascii") for i in range(8)]
        replies = self.race([("SET", key, value, "GET", "KEEPTTL") for value in values])
        final = self.cmd("GET", key)
        self.assertEqual(Counter(replies + [final]), Counter([b"initial"] + values))
        self.assert_kept_ttl(key, before, time.monotonic() - started)
        nx = self.key("nx")
        replies = self.race([("SET", nx, value, "NX", "GET") for value in values])
        winner = self.cmd("GET", nx)
        self.assertEqual(replies.count(None), 1)
        self.assertEqual(Counter(replies), Counter({None: 1, winner: 7}))
        self.assertEqual(winner, values[replies.index(None)])

    def persistence_roundtrip(self, aof):
        if self.server is None:
            self.skipTest("Restart/persistence tests require --server")
        keep, cleared = self.key("keep"), self.key("cleared")
        changed, removed = self.key("changed"), self.key("removed")
        setat, goneat = self.key("setat"), self.key("goneat")
        conditional, rejected = self.key("conditional"), self.key("rejected")
        binary = b"\x00persist\r\n\xff"
        self.cmd("SET", keep, "old", "PX", 180000)
        self.assertEqual(self.cmd("SET", keep, binary, "GET", "KEEPTTL"), b"old")
        self.cmd("SET", cleared, "persistent", "PX", 180000)
        self.assertEqual(self.cmd("GETEX", cleared, "PERSIST"), b"persistent")
        self.cmd("SET", changed, "changed")
        self.cmd("GETEX", changed, "EXAT", self.now_ms() // 1000 + 180)
        self.cmd("SET", removed, "deleted")
        self.assertEqual(self.cmd("GETDEL", removed), b"deleted")
        self.cmd("SET", setat, "absolute", "PXAT", self.now_ms() + 180000)
        self.cmd("SET", goneat, "past", "EXAT", 1)
        getexpast = self.key("getexpast")
        self.cmd("SET", getexpast, "past")
        self.assertEqual(self.cmd("GETEX", getexpast, "PXAT", 1), b"past")
        expirepast = self.key("expirepast")
        self.cmd("SET", expirepast, "past", "PX", 180000)
        self.assertEqual(self.cmd("PEXPIRE", expirepast, 0, "XX", "LT"), 1)
        self.cmd("SET", conditional, "condition")
        self.assertEqual(self.cmd("PEXPIRE", conditional, 180000, "NX"), 1)
        self.assertEqual(self.cmd("PEXPIRE", conditional, 60000, "XX", "LT"), 1)
        self.cmd("SET", rejected, "unchanged", "PX", 180000)
        self.assertEqual(self.cmd("SET", rejected, "no", "NX", "GET"), b"unchanged")
        self.assertEqual(self.cmd("PEXPIRE", rejected, 300000, "LT"), 0)
        self.error_unchanged(rejected, ("GETEX", rejected, "PX", LLONG_MAX))
        # Restart after some wall time so accidentally replaying a relative TTL
        # gives a detectably later deadline. AOF uses fsync=always, no SAVE.
        expiring = (keep, changed, setat, conditional, rejected)
        deadlines = {key: self.now_ms() + self.cmd("PTTL", key) for key in expiring}
        if not aof:
            self.assertEqual(self.cmd("SAVE"), b"OK")
            self.assertTrue((self.server.directory / "dump.rdb").is_file())
        time.sleep(1.15)
        self.restart()
        for key, value in ((keep, binary), (cleared, b"persistent"),
                           (changed, b"changed"), (setat, b"absolute"),
                           (conditional, b"condition"), (rejected, b"unchanged")):
            self.assertEqual(self.cmd("GET", key), value)
        self.assertEqual(self.cmd("PTTL", cleared), -1)
        for key in (removed, goneat, getexpast, expirepast):
            self.assertIsNone(self.cmd("GET", key))
            self.assertEqual(self.cmd("PTTL", key), -2)
        for key, deadline in deadlines.items():
            self.assertGreater(self.cmd("PTTL", key), 0)
            restored = self.now_ms() + self.cmd("PTTL", key)
            self.assertLessEqual(abs(restored - deadline), 350,
                                 "Deadline changed across restart for %r: %d ms" %
                                 (key, restored - deadline))
        if aof:
            # Both Redis 5's single file and Redis 7's multipart directory work.
            self.assertTrue(any(fnmatch.fnmatch(path.name, "*.aof*")
                                for path in self.server.directory.rglob("*")))

    def test_rdb_save_restart(self):
        self.persistence_roundtrip(aof=False)

    def test_aof_restart(self):
        self.persistence_roundtrip(aof=True)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--server", help="Redis executable; launch isolated per-test instances")
    target.add_argument("--host", help="Connect to an existing instance; use unique test keys")
    parser.add_argument("--port", type=int, default=6379, help="Existing instance port (default: 6379)")
    parser.add_argument("--password", help="AUTH password for an existing instance")
    parser.add_argument("--timeout", type=float, default=5.0, help="Socket/worker timeout in seconds")
    parser.add_argument("--startup-timeout", type=float, default=15.0,
                        help="Managed instance startup timeout in seconds")
    parser.add_argument("--match", action="append", default=[], help="Only names containing this substring")
    parser.add_argument("--list", action="store_true", help="List selected tests without connecting")
    parser.add_argument("--failfast", action="store_true", help="Stop after the first failure/error")
    args = parser.parse_args(argv)
    if not args.list and not (args.server or args.host):
        parser.error("Specify --server PATH or --host HOST (with optional --port)")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.timeout <= 0 or args.startup_timeout <= 0:
        parser.error("Timeouts must be positive")
    if args.server:
        if args.password is not None:
            parser.error("--password is only for an existing instance")
        path = Path(args.server).expanduser().resolve()
        if not path.is_file():
            parser.error("Server executable not found: %s" % path)
        args.server = str(path)
    return args


def main(argv=None):
    global OPTIONS
    OPTIONS = parse_args(argv)
    names = unittest.defaultTestLoader.getTestCaseNames(BackportRegression)
    names = [name for name in names if not OPTIONS.match or
             any(fragment in name for fragment in OPTIONS.match)]
    if not names:
        print("No tests matched", file=sys.stderr)
        return 2
    if OPTIONS.list:
        for name in names:
            print(name)
        return 0
    mode = "managed per-test servers" if OPTIONS.server else "external instance; unique key prefixes"
    print("Running %d tests (%s)" % (len(names), mode), flush=True)
    suite = unittest.TestSuite(BackportRegression(name) for name in names)
    previous_term = signal.getsignal(signal.SIGTERM)

    def interrupted(signum, frame):
        raise KeyboardInterrupt("Signal %d" % signum)

    signal.signal(signal.SIGTERM, interrupted)
    try:
        result = unittest.TextTestRunner(verbosity=2, failfast=OPTIONS.failfast).run(suite)
        return 0 if result.wasSuccessful() else 1
    except KeyboardInterrupt:
        print("\nInterrupted; cleaning up owned servers", file=sys.stderr)
        return 130
    finally:
        try:
            cleanup_servers()
        finally:
            signal.signal(signal.SIGTERM, previous_term)


if __name__ == "__main__":
    sys.exit(main())
