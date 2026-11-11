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
import select
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
    def __init__(self, executable, directory, timeout, startup_timeout, aof=False,
                 requirepass=None, aclfile=False):
        self.executable = executable
        self.directory = Path(directory)
        self.timeout = timeout
        self.startup_timeout = startup_timeout
        self.aof = aof
        self.requirepass = requirepass
        self.aclfile = aclfile
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
        aclfile_line = ""
        if self.aclfile:
            acl = self.directory / "acl.conf"
            acl.write_text(
                "user default on %s ~* &* +@all\n" % (
                    ">%s" % self.requirepass if self.requirepass else "nopass"),
                encoding="ascii",
            )
            aclfile_line = "aclfile %s\n" % acl.as_posix()
        config.write_text(
            "bind 127.0.0.1\nprotected-mode yes\nport %d\n"
            "daemonize no\nsave \"\"\ndir .\ndbfilename dump.rdb\n"
            "logfile server.log\nloglevel notice\n"
            "appendonly %s\nappendfilename appendonly.aof\n"
            "appendfsync always\naof-use-rdb-preamble no\n"
            "auto-aof-rewrite-percentage 0\n%s%s" % (
                self.port, "yes" if self.aof else "no",
                "requirepass %s\n" % self.requirepass if self.requirepass else "",
                aclfile_line),
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
                    with RESP2("127.0.0.1", self.port, min(self.timeout, 0.5),
                                self.requirepass) as client:
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
                aclfile="aclfile" in self._testMethodName,
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

    @staticmethod
    def flat_map(reply):
        """Convert a flat RESP2 map (field, value, field, value, ...) to a dict."""
        return {reply[i]: reply[i + 1] for i in range(0, len(reply), 2)}

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

    def test_zunion_zinter_zdiff(self):
        self.cmd("DEL", "za", "zb", "out")
        self.cmd("ZADD", "za", "1", "a", "2", "b")
        self.cmd("ZADD", "zb", "3", "b", "4", "c")
        union = self.cmd("ZUNION", "2", "za", "zb")
        self.assertCountEqual(union, [b"a", b"b", b"c"])
        scored = self.cmd("ZUNION", "2", "za", "zb", "WITHSCORES")
        self.assertEqual(scored, [b"a", b"1", b"c", b"4", b"b", b"5"])
        self.assertEqual(self.cmd("ZINTER", "2", "za", "zb", "WITHSCORES"),
                         [b"b", b"5"])
        self.assertEqual(self.cmd("ZDIFF", "2", "za", "zb"), [b"a"])
        self.assertEqual(self.cmd("ZDIFF", "2", "zb", "za"), [b"c"])
        # The read-only forms must not write anything.
        self.assertEqual(self.cmd("EXISTS", "out"), 0)
        self.assertEqual(self.cmd("ZDIFFSTORE", "out", "2", "za", "zb"), 1)
        self.assertEqual(self.cmd("ZRANGE", "out", "0", "-1"), [b"a"])
        self.cmd("SET", "plain", "x")
        with self.assertRaises(RedisError):
            self.cmd("ZUNION", "2", "za", "plain")

    def test_intercard_and_lazyfree_options(self):
        self.cmd("DEL", "sa", "sb", "sc", "za", "zb", "zc", "lz")
        self.cmd("SADD", "sa", "a", "b", "c")
        self.cmd("SADD", "sb", "b", "c", "d")
        self.assertEqual(self.cmd("SINTERCARD", "2", "sa", "sb"), 2)
        self.assertEqual(self.cmd("SINTERCARD", "2", "sa", "sb", "LIMIT", "1"), 1)
        self.assertEqual(self.cmd("SINTERCARD", "3", "sa", "sb", "sc"), 0)
        self.assertEqual(self.cmd("SINTERCARD", "2", "sa", "missing"), 0)
        with self.assertRaises(RedisError):
            self.cmd("SINTERCARD", "0")
        with self.assertRaises(RedisError):
            self.cmd("SINTERCARD", "3", "sa", "sb")
        with self.assertRaises(RedisError):
            self.cmd("SINTERCARD", "2", "sa", "sb", "LIMIT", "-1")
        self.cmd("SET", "lz", "x")
        with self.assertRaises(RedisError):
            self.cmd("SINTERCARD", "2", "sa", "lz")

        self.cmd("ZADD", "za", "1", "a", "2", "b", "3", "c")
        self.cmd("ZADD", "zb", "9", "b", "8", "c", "7", "d")
        self.assertEqual(self.cmd("ZINTERCARD", "2", "za", "zb"), 2)
        self.assertEqual(self.cmd("ZINTERCARD", "2", "za", "zb", "LIMIT", "1"), 1)
        self.assertEqual(self.cmd("ZINTERCARD", "2", "za", "missing"), 0)
        with self.assertRaises(RedisError):
            self.cmd("ZINTERCARD", "2", "za", "lz")

        # The two Redis 6 lazyfree options apply to DEL and FLUSHDB.
        self.assertEqual(self.cmd("CONFIG", "SET", "lazyfree-lazy-user-del", "yes"),
                         b"OK")
        self.assertEqual(self.cmd("CONFIG", "GET", "lazyfree-lazy-user-del"),
                         [b"lazyfree-lazy-user-del", b"yes"])
        self.cmd("DEL", "lz")
        self.cmd("RPUSH", "lz", "a", "b", "c")
        self.assertEqual(self.cmd("DEL", "lz"), 1)
        self.assertEqual(self.cmd("EXISTS", "lz"), 0)
        self.cmd("CONFIG", "SET", "lazyfree-lazy-user-del", "no")

        self.assertEqual(self.cmd("CONFIG", "SET", "lazyfree-lazy-user-flush", "yes"),
                         b"OK")
        self.assertEqual(self.cmd("CONFIG", "GET", "lazyfree-lazy-user-flush"),
                         [b"lazyfree-lazy-user-flush", b"yes"])
        self.cmd("SET", "fl", "v")
        self.assertEqual(self.cmd("FLUSHDB"), b"OK")
        self.assertEqual(self.cmd("DBSIZE"), 0)
        self.cmd("CONFIG", "SET", "lazyfree-lazy-user-flush", "no")

    def test_lpos_and_lmove(self):
        self.cmd("DEL", "lpos", "lsrc", "ldst", "lplain")
        self.cmd("RPUSH", "lpos", "a", "b", "c", "b", "a")
        self.assertEqual(self.cmd("LPOS", "lpos", "b"), 1)
        self.assertEqual(self.cmd("LPOS", "lpos", "b", "RANK", "-1"), 3)
        self.assertEqual(self.cmd("LPOS", "lpos", "b", "COUNT", "0"), [1, 3])
        self.assertEqual(self.cmd("LPOS", "lpos", "b", "COUNT", "1"), [1])
        self.assertEqual(self.cmd("LPOS", "lpos", "b", "RANK", "-1", "COUNT", "1"),
                         [3])
        # MAXLEN bounds how many elements are examined from the chosen end.
        self.assertIsNone(self.cmd("LPOS", "lpos", "b", "MAXLEN", "1"))
        self.assertEqual(self.cmd("LPOS", "lpos", "b", "MAXLEN", "2"), 1)
        self.assertIsNone(self.cmd("LPOS", "lpos", "missing"))
        self.assertEqual(self.cmd("LPOS", "lpos", "missing", "COUNT", "0"), [])
        self.assertIsNone(self.cmd("LPOS", "lpos", "zzz"))
        self.assertEqual(self.cmd("LPOS", "lpos", "zzz", "COUNT", "0"), [])
        with self.assertRaises(RedisError):
            self.cmd("LPOS", "lpos", "b", "RANK", "0")
        with self.assertRaises(RedisError):
            self.cmd("LPOS", "lpos", "b", "COUNT", "-1")
        with self.assertRaises(RedisError):
            self.cmd("LPOS", "lpos", "b", "MAXLEN", "-1")

        self.cmd("RPUSH", "lsrc", "a", "b", "c")
        self.assertEqual(self.cmd("LMOVE", "lsrc", "ldst", "LEFT", "RIGHT"), b"a")
        self.assertEqual(self.cmd("LRANGE", "lsrc", "0", "-1"), [b"b", b"c"])
        self.assertEqual(self.cmd("LRANGE", "ldst", "0", "-1"), [b"a"])
        self.assertEqual(self.cmd("LMOVE", "lsrc", "ldst", "RIGHT", "LEFT"), b"c")
        self.assertEqual(self.cmd("LRANGE", "ldst", "0", "-1"), [b"c", b"a"])
        # Source and destination may be the same list.
        self.cmd("DEL", "lrot")
        self.cmd("RPUSH", "lrot", "1", "2", "3")
        self.assertEqual(self.cmd("LMOVE", "lrot", "lrot", "LEFT", "RIGHT"), b"1")
        self.assertEqual(self.cmd("LRANGE", "lrot", "0", "-1"),
                         [b"2", b"3", b"1"])
        self.assertIsNone(self.cmd("LMOVE", "missing", "ldst", "LEFT", "RIGHT"))
        with self.assertRaises(RedisError):
            self.cmd("LMOVE", "lsrc", "ldst", "MIDDLE", "RIGHT")
        self.cmd("SET", "lplain", "x")
        with self.assertRaises(RedisError):
            self.cmd("LPOS", "lplain", "x")
        with self.assertRaises(RedisError):
            self.cmd("LMOVE", "lplain", "ldst", "LEFT", "RIGHT")
        self.cmd("DEL", "lpos", "lsrc", "ldst", "lrot", "lplain")

    def test_zadd_gt_lt_and_random_members(self):
        self.cmd("DEL", "zgt", "zplain", "hr", "hrbig", "zr", "zrbig")
        self.cmd("ZADD", "zgt", "10", "a", "20", "b")
        # GT/LT never create a missing element differently: new elements are
        # always added, the option only filters updates of existing ones.
        self.assertEqual(self.cmd("ZADD", "zgt", "GT", "30", "new"), 1)
        self.assertEqual(self.cmd("ZADD", "zgt", "LT", "1", "new2"), 1)
        self.assertEqual(self.cmd("ZADD", "zgt", "GT", "5", "a"), 0)
        self.assertEqual(self.cmd("ZSCORE", "zgt", "a"), b"10")
        self.assertEqual(self.cmd("ZADD", "zgt", "GT", "CH", "15", "a"), 1)
        self.assertEqual(self.cmd("ZSCORE", "zgt", "a"), b"15")
        self.assertEqual(self.cmd("ZADD", "zgt", "GT", "CH", "15", "a"), 0)
        self.assertEqual(self.cmd("ZADD", "zgt", "LT", "CH", "20", "a"), 0)
        self.assertEqual(self.cmd("ZSCORE", "zgt", "a"), b"15")
        self.assertEqual(self.cmd("ZADD", "zgt", "LT", "CH", "5", "a"), 1)
        self.assertEqual(self.cmd("ZSCORE", "zgt", "a"), b"5")
        # INCR returns nil when GT/LT rejects the resulting score.
        self.assertIsNone(self.cmd("ZADD", "zgt", "INCR", "GT", "-1", "a"))
        self.assertEqual(self.cmd("ZSCORE", "zgt", "a"), b"5")
        self.assertEqual(self.cmd("ZADD", "zgt", "INCR", "GT", "10", "a"),
                         b"15")
        with self.assertRaises(RedisError):
            self.cmd("ZADD", "zgt", "GT", "LT", "1", "a")
        with self.assertRaises(RedisError):
            self.cmd("ZADD", "zgt", "GT", "NX", "1", "a")
        with self.assertRaises(RedisError):
            self.cmd("ZADD", "zgt", "LT", "NX", "1", "a")
        self.cmd("SET", "zplain", "x")
        with self.assertRaises(RedisError):
            self.cmd("ZADD", "zplain", "GT", "1", "a")

        # HRANDFIELD: single, unique count, repetition and WITHVALUES.
        self.cmd("HSET", "hr", "f1", "v1", "f2", "v2", "f3", "v3")
        self.assertIn(self.cmd("HRANDFIELD", "hr"), (b"f1", b"f2", b"f3"))
        sampled = self.cmd("HRANDFIELD", "hr", "2")
        self.assertEqual(len(sampled), 2)
        self.assertEqual(len(set(sampled)), 2)
        pairs = self.cmd("HRANDFIELD", "hr", "2", "WITHVALUES")
        self.assertEqual(len(pairs), 4)
        self.assertEqual(len(set(pairs[0::2])), 2)
        for field, value in zip(pairs[0::2], pairs[1::2]):
            self.assertEqual(value, b"v" + field[1:])
        self.assertEqual(len(self.cmd("HRANDFIELD", "hr", "-5")), 5)
        self.assertEqual(self.cmd("HRANDFIELD", "hr", "0"), [])
        self.assertCountEqual(self.cmd("HRANDFIELD", "hr", "10"),
                              [b"f1", b"f2", b"f3"])
        self.assertEqual(len(self.cmd("HRANDFIELD", "hr", "10", "WITHVALUES")), 6)
        self.assertIsNone(self.cmd("HRANDFIELD", "missing"))
        self.assertEqual(self.cmd("HRANDFIELD", "missing", "3"), [])
        with self.assertRaises(RedisError):
            self.cmd("HRANDFIELD", "hr", "1", "WITHSCORES")
        with self.assertRaises(RedisError):
            self.cmd("HRANDFIELD", "zplain")

        # Hashes above hash-max-ziplist-entries use the hash table encoding.
        for i in range(600):
            self.cmd("HSET", "hrbig", "field:%d" % i, "value:%d" % i)
        big = self.cmd("HRANDFIELD", "hrbig", "50")
        self.assertEqual(len(big), 50)
        self.assertEqual(len(set(big)), 50)
        bigpairs = self.cmd("HRANDFIELD", "hrbig", "50", "WITHVALUES")
        for field, value in zip(bigpairs[0::2], bigpairs[1::2]):
            self.assertEqual(value, field.replace(b"field:", b"value:", 1))

        # ZRANDMEMBER: same count semantics plus WITHSCORES.
        self.cmd("ZADD", "zr", "1", "a", "2", "b", "3", "c")
        self.assertIn(self.cmd("ZRANDMEMBER", "zr"), (b"a", b"b", b"c"))
        sampled = self.cmd("ZRANDMEMBER", "zr", "2")
        self.assertEqual(len(sampled), 2)
        self.assertEqual(len(set(sampled)), 2)
        scored = self.cmd("ZRANDMEMBER", "zr", "2", "WITHSCORES")
        self.assertEqual(len(scored), 4)
        expected = {b"a": b"1", b"b": b"2", b"c": b"3"}
        for member, score in zip(scored[0::2], scored[1::2]):
            self.assertEqual(score, expected[member])
        self.assertEqual(len(self.cmd("ZRANDMEMBER", "zr", "-4")), 4)
        self.assertEqual(self.cmd("ZRANDMEMBER", "zr", "0"), [])
        self.assertCountEqual(self.cmd("ZRANDMEMBER", "zr", "9"),
                              [b"a", b"b", b"c"])
        self.assertEqual(len(self.cmd("ZRANDMEMBER", "zr", "9", "WITHSCORES")), 6)
        self.assertIsNone(self.cmd("ZRANDMEMBER", "missing"))
        self.assertEqual(self.cmd("ZRANDMEMBER", "missing", "2"), [])
        with self.assertRaises(RedisError):
            self.cmd("ZRANDMEMBER", "zr", "1", "WITHVALUES")
        with self.assertRaises(RedisError):
            self.cmd("ZRANDMEMBER", "zplain")

        # Sorted sets above zset-max-ziplist-entries use the skiplist encoding.
        for i in range(200):
            self.cmd("ZADD", "zrbig", str(i), "member:%d" % i)
        big = self.cmd("ZRANDMEMBER", "zrbig", "50", "WITHSCORES")
        self.assertEqual(len(big), 100)
        members = big[0::2]
        self.assertEqual(len(set(members)), 50)
        for member, score in zip(members, big[1::2]):
            self.assertEqual(score, member.split(b":")[1])
        self.cmd("DEL", "zgt", "zplain", "hr", "hrbig", "zr", "zrbig")

    def test_lpop_count_and_blmove(self):
        self.cmd("DEL", "lc", "blsrc", "bldst", "blstr")
        self.cmd("RPUSH", "lc", "a", "b", "c", "d")
        self.assertEqual(self.cmd("LPOP", "lc", "2"), [b"a", b"b"])
        self.assertEqual(self.cmd("LRANGE", "lc", "0", "-1"), [b"c", b"d"])
        self.assertEqual(self.cmd("RPOP", "lc", "5"), [b"d", b"c"])
        self.assertEqual(self.cmd("EXISTS", "lc"), 0)
        self.assertIsNone(self.cmd("LPOP", "missing", "2"))
        with self.assertRaises(RedisError):
            self.cmd("LPOP", "lc", "-1")
        self.cmd("RPUSH", "lc", "x")
        self.assertEqual(self.cmd("LPOP", "lc", "0"), [])
        self.assertEqual(self.cmd("LRANGE", "lc", "0", "-1"), [b"x"])
        self.assertEqual(self.cmd("LPOP", "lc", "1"), [b"x"])
        self.assertEqual(self.cmd("EXISTS", "lc"), 0)

        self.cmd("RPUSH", "blsrc", "a", "b")
        self.assertEqual(self.cmd("BLMOVE", "blsrc", "bldst", "LEFT", "RIGHT", "0"),
                         b"a")
        self.assertEqual(self.cmd("LRANGE", "bldst", "0", "-1"), [b"a"])
        self.assertEqual(self.cmd("BLMOVE", "blsrc", "bldst", "RIGHT", "LEFT", "0"),
                         b"b")
        self.assertEqual(self.cmd("LRANGE", "bldst", "0", "-1"), [b"b", b"a"])

        blocked = self.connect()
        try:
            blocked.sock.sendall(blocked.encode(
                ("BLMOVE", "blsrc", "bldst", "LEFT", "RIGHT", "5")))
            time.sleep(0.2)
            self.cmd("RPUSH", "blsrc", "wake")
            self.assertEqual(blocked._read(), b"wake")
        finally:
            blocked.close()
        self.assertEqual(self.cmd("LRANGE", "bldst", "0", "-1"),
                         [b"b", b"a", b"wake"])
        self.assertIsNone(self.cmd("BLMOVE", "missing", "bldst", "LEFT", "RIGHT",
                                   "0.1"))
        self.cmd("SET", "blstr", "x")
        with self.assertRaises(RedisError):
            self.cmd("BLMOVE", "bldst", "blstr", "LEFT", "RIGHT", "0")
        self.assertEqual(self.cmd("LLEN", "bldst"), 3)
        self.assertEqual(self.cmd("BRPOPLPUSH", "bldst", "blsrc", "0"), b"wake")
        self.assertEqual(self.cmd("LRANGE", "blsrc", "0", "-1"), [b"wake"])
        self.cmd("DEL", "lc", "blsrc", "bldst", "blstr")

    def test_unified_zrange_and_zrangestore(self):
        self.cmd("DEL", "zr", "zd", "zplain")
        self.cmd("ZADD", "zr", "1", "a", "2", "b", "3", "c", "4", "d")
        self.assertEqual(self.cmd("ZRANGE", "zr", "0", "-1"),
                         [b"a", b"b", b"c", b"d"])
        self.assertEqual(self.cmd("ZRANGE", "zr", "1", "2"), [b"b", b"c"])
        self.assertEqual(self.cmd("ZRANGE", "zr", "0", "-1", "REV"),
                         [b"d", b"c", b"b", b"a"])
        self.assertEqual(self.cmd("ZRANGE", "zr", "0", "1", "WITHSCORES"),
                         [b"a", b"1", b"b", b"2"])
        self.assertEqual(self.cmd("ZRANGE", "zr", "(1", "3", "BYSCORE"),
                         [b"b", b"c"])
        self.assertEqual(self.cmd("ZRANGE", "zr", "3", "(1", "BYSCORE", "REV"),
                         [b"c", b"b"])
        self.assertEqual(self.cmd("ZRANGE", "zr", "-inf", "+inf", "BYSCORE",
                                  "LIMIT", "1", "2"), [b"b", b"c"])
        self.assertEqual(self.cmd("ZRANGE", "zr", "(a", "[c", "BYLEX"),
                         [b"b", b"c"])
        self.assertEqual(self.cmd("ZRANGE", "zr", "[c", "(a", "BYLEX", "REV"),
                         [b"c", b"b"])
        self.assertEqual(self.cmd("ZRANGE", "zr", "-", "+", "BYLEX", "LIMIT",
                                  "1", "2"), [b"b", b"c"])
        with self.assertRaises(RedisError):
            self.cmd("ZRANGE", "zr", "0", "-1", "LIMIT", "0", "1")
        with self.assertRaises(RedisError):
            self.cmd("ZRANGE", "zr", "-", "+", "BYLEX", "WITHSCORES")
        with self.assertRaises(RedisError):
            self.cmd("ZRANGE", "zr", "0", "-1", "NOPE")
        self.assertEqual(self.cmd("ZRANGE", "missing", "0", "-1"), [])

        self.assertEqual(self.cmd("ZRANGESTORE", "zd", "zr", "0", "-1"), 4)
        self.assertEqual(self.cmd("ZRANGE", "zd", "0", "-1"),
                         [b"a", b"b", b"c", b"d"])
        self.assertEqual(self.cmd("ZRANGESTORE", "zd", "zr", "2", "3",
                                  "BYSCORE"), 2)
        self.assertEqual(self.cmd("ZRANGE", "zd", "0", "-1", "WITHSCORES"),
                         [b"b", b"2", b"c", b"3"])
        self.assertEqual(self.cmd("ZRANGESTORE", "zd", "zr", "5", "10",
                                  "BYSCORE"), 0)
        self.assertEqual(self.cmd("EXISTS", "zd"), 0)
        self.assertEqual(self.cmd("ZRANGESTORE", "zd", "missing", "0", "-1"), 0)
        self.assertEqual(self.cmd("EXISTS", "zd"), 0)
        self.assertEqual(self.cmd("ZRANGESTORE", "zd", "zr", "(a", "[c",
                                  "BYLEX"), 2)
        self.assertEqual(self.cmd("ZRANGE", "zd", "0", "-1"), [b"b", b"c"])
        with self.assertRaises(RedisError):
            self.cmd("ZRANGESTORE", "zd", "zr", "0", "-1", "WITHSCORES")
        self.cmd("SET", "zplain", "x")
        with self.assertRaises(RedisError):
            self.cmd("ZRANGESTORE", "zd", "zplain", "0", "-1")

        self.assertEqual(self.cmd("ZREVRANGE", "zr", "0", "-1"),
                         [b"d", b"c", b"b", b"a"])
        self.assertEqual(self.cmd("ZRANGEBYSCORE", "zr", "2", "3"), [b"b", b"c"])
        self.assertEqual(self.cmd("ZREVRANGEBYSCORE", "zr", "3", "2",
                                  "WITHSCORES"),
                         [b"c", b"3", b"b", b"2"])
        self.assertEqual(self.cmd("ZRANGEBYLEX", "zr", "-", "+", "LIMIT", "0",
                                  "2"), [b"a", b"b"])
        self.assertEqual(self.cmd("ZCOUNT", "zr", "2", "3"), 2)
        self.assertEqual(self.cmd("ZLEXCOUNT", "zr", "-", "+"), 4)
        self.cmd("DEL", "zr", "zd", "zplain")

    def test_copy_and_reset(self):
        self.cmd("DEL", "src", "dst")
        self.cmd("SET", "src", "value", "EX", "100")
        self.assertEqual(self.cmd("COPY", "src", "dst"), 1)
        self.assertEqual(self.cmd("GET", "dst"), b"value")
        self.assertGreater(self.cmd("TTL", "dst"), 0)
        # Without REPLACE an existing destination is left untouched.
        self.cmd("SET", "dst", "kept")
        self.assertEqual(self.cmd("COPY", "src", "dst"), 0)
        self.assertEqual(self.cmd("GET", "dst"), b"kept")
        self.assertEqual(self.cmd("COPY", "src", "dst", "REPLACE"), 1)
        self.assertEqual(self.cmd("GET", "dst"), b"value")
        self.assertEqual(self.cmd("COPY", "missing", "dst"), 0)
        with self.assertRaises(RedisError):
            self.cmd("COPY", "src", "src")
        # Every non-string type is a real deep copy: deleting one key must
        # leave the other intact, since those values share no refcount.
        self.cmd("DEL", "src", "dst")
        self.cmd("RPUSH", "src", "a", "b")
        self.assertEqual(self.cmd("COPY", "src", "dst"), 1)
        self.cmd("DEL", "src")
        self.assertEqual(self.cmd("LRANGE", "dst", "0", "-1"), [b"a", b"b"])
        self.cmd("DEL", "src", "dst")
        self.cmd("SADD", "src", "1", "2")
        self.assertEqual(self.cmd("COPY", "src", "dst"), 1)
        self.cmd("DEL", "src")
        self.assertEqual(sorted(self.cmd("SMEMBERS", "dst")), [b"1", b"2"])
        self.cmd("DEL", "src", "dst")
        self.cmd("ZADD", "src", "1", "a", "2", "b")
        self.assertEqual(self.cmd("COPY", "src", "dst"), 1)
        self.cmd("DEL", "src")
        self.assertEqual(self.cmd("ZRANGE", "dst", "0", "-1", "WITHSCORES"),
                         [b"a", b"1", b"b", b"2"])
        self.cmd("DEL", "src", "dst")
        self.cmd("HSET", "src", "f", "v")
        self.assertEqual(self.cmd("COPY", "src", "dst"), 1)
        self.cmd("DEL", "src")
        self.assertEqual(self.cmd("HGETALL", "dst"), [b"f", b"v"])
        self.cmd("DEL", "src", "dst")
        self.cmd("XADD", "src", "1-0", "f", "v")
        self.assertEqual(self.cmd("COPY", "src", "dst"), 1)
        self.cmd("DEL", "src")
        self.assertEqual(self.cmd("XRANGE", "dst", "-", "+"),
                         [[b"1-0", [b"f", b"v"]]])
        self.cmd("DEL", "dst")
        # RESET returns to database 0 and drops the client name.
        self.cmd("SELECT", "1")
        self.cmd("CLIENT", "SETNAME", "worker")
        self.assertEqual(self.cmd("RESET"), b"OK")
        self.assertEqual(self.cmd("CLIENT", "GETNAME"), None)
        self.cmd("SET", "after-reset", "1")
        self.cmd("SELECT", "1")
        self.assertIsNone(self.cmd("GET", "after-reset"))
        self.cmd("SELECT", "0")
        self.cmd("DEL", "after-reset")

    def test_acl_users_commands_and_keys(self):
        # requirepass is what makes an unauthenticated connection fail closed.
        # Without it every connection is the default user and may run anything.
        guarded_dir = Path(self.temp.name) / "acl"
        guarded_dir.mkdir()
        guarded = ManagedServer(OPTIONS.server, guarded_dir, OPTIONS.timeout,
                                OPTIONS.startup_timeout, requirepass="admin")
        self.addCleanup(guarded.close)
        guarded.start()
        admin = RESP2("127.0.0.1", guarded.port, OPTIONS.timeout, "admin")
        self.addCleanup(admin.close)
        self.addCleanup(admin.command, "ACL", "DELUSER", "reader")
        self.assertEqual(admin.command("ACL", "SETUSER", "reader", "on", ">secret",
                                  "resetkeys", "~cache:*", "-@all", "+get", "+ping",
                                  "+sintercard"), b"OK")
        # A fresh connection, so it does not share the default user's rights.
        limited = RESP2("127.0.0.1", guarded.port, OPTIONS.timeout)
        self.addCleanup(limited.close)
        with self.assertRaises(RedisError):
            limited.command("GET", "cache:1")
        self.assertEqual(limited.command("AUTH", "reader", "secret"), b"OK")
        self.assertEqual(limited.command("ACL", "WHOAMI"), b"reader")
        admin.command("SET", "cache:1", "v")
        admin.command("SET", "other", "v")
        self.assertEqual(limited.command("GET", "cache:1"), b"v")
        # The key pattern excludes this key, and SET is not in the command set.
        with self.assertRaises(RedisError):
            limited.command("GET", "other")
        with self.assertRaises(RedisError):
            limited.command("SET", "cache:1", "no")
        # A numkeys command must have all of its keys checked, not just the
        # first one or the count argument.
        self.assertEqual(limited.command("SINTERCARD", "2", "cache:3", "cache:4"), 0)
        with self.assertRaises(RedisError):
            limited.command("SINTERCARD", "2", "cache:1", "other")
        self.assertEqual(limited.command("PING"), b"PONG")
        limited.close()
        denied = RESP2("127.0.0.1", guarded.port, OPTIONS.timeout)
        self.addCleanup(denied.close)
        with self.assertRaises(RedisError):
            denied.command("AUTH", "reader", "wrong")

    def test_lmpop_and_blmpop(self):
        lkey = self.key("lmpop:list")
        lkey2 = self.key("lmpop:other")
        missing = self.key("lmpop:missing")
        plain = self.key("lmpop:string")
        self.cmd("DEL", lkey, lkey2, missing, plain)
        self.cmd("RPUSH", lkey, "a", "b", "c", "d", "e")

        # LMPOP replies with [key, [elements...]] and skips missing keys.
        self.assertEqual(self.cmd("LMPOP", "2", missing, lkey, "LEFT", "COUNT", "2"),
                         [lkey, [b"a", b"b"]])
        self.assertEqual(self.cmd("LRANGE", lkey, "0", "-1"), [b"c", b"d", b"e"])
        # COUNT defaults to one and is capped by the list length.
        self.assertEqual(self.cmd("LMPOP", "1", lkey, "RIGHT"), [lkey, [b"e"]])
        self.assertEqual(self.cmd("LMPOP", "1", lkey, "RIGHT", "COUNT", "10"),
                         [lkey, [b"d", b"c"]])
        self.assertEqual(self.cmd("EXISTS", lkey), 0)
        # No key had elements: a null array, not an empty one.
        self.assertIsNone(self.cmd("LMPOP", "1", missing, "LEFT"))
        self.assertIsNone(self.cmd("LMPOP", "2", missing, lkey, "LEFT"))

        with self.assertRaises(RedisError):
            self.cmd("LMPOP", "0", lkey, "LEFT")
        with self.assertRaises(RedisError):
            self.cmd("LMPOP", "99999", lkey, "LEFT")
        with self.assertRaises(RedisError):
            self.cmd("LMPOP", "9223372036854775807", lkey, "LEFT")
        with self.assertRaises(RedisError):
            self.cmd("LMPOP", "1", lkey, "LEFT", "COUNT", "0")
        with self.assertRaises(RedisError):
            self.cmd("LMPOP", "1", lkey, "SIDEWAYS")
        with self.assertRaises(RedisError):
            self.cmd("LMPOP", "1", lkey, "LEFT", "COUNT")
        with self.assertRaises(RedisError):
            self.cmd("LMPOP", "1", lkey, "LEFT", "COUNT", "1", "COUNT", "2")
        self.cmd("SET", plain, "x")
        with self.assertRaises(RedisError):
            self.cmd("LMPOP", "1", plain, "LEFT")

        # BLMPOP serves an already non-empty list immediately.
        self.cmd("RPUSH", lkey2, "a", "b", "c")
        self.assertEqual(self.cmd("BLMPOP", "0", "1", lkey2, "RIGHT", "COUNT", "2"),
                         [lkey2, [b"c", b"b"]])
        self.assertEqual(self.cmd("LRANGE", lkey2, "0", "-1"), [b"a"])

        # BLMPOP waits for a push on any of its keys.
        blkey = self.key("lmpop:block")
        self.cmd("DEL", blkey)
        blocked = self.connect()
        try:
            blocked.sock.sendall(blocked.encode(
                ("BLMPOP", "5", "2", missing, blkey, "LEFT", "COUNT", "2")))
            time.sleep(0.2)
            self.cmd("RPUSH", blkey, "w", "x", "y")
            self.assertEqual(blocked._read(), [blkey, [b"w", b"x"]])
        finally:
            blocked.close()
        self.assertEqual(self.cmd("LRANGE", blkey, "0", "-1"), [b"y"])

        # A blocking pop that empties the list deletes the key.
        blocked = self.connect()
        try:
            blocked.sock.sendall(blocked.encode(
                ("BLMPOP", "5", "1", blkey, "RIGHT", "COUNT", "5")))
            time.sleep(0.2)
            self.assertEqual(blocked._read(), [blkey, [b"y"]])
        finally:
            blocked.close()
        self.assertEqual(self.cmd("EXISTS", blkey), 0)

        # Timeout and MULTI/EXEC both reply with a null array.
        self.assertIsNone(self.cmd("BLMPOP", "0.1", "1", missing, "LEFT"))
        self.assertEqual(self.cmd("MULTI"), b"OK")
        self.assertEqual(self.cmd("BLMPOP", "0", "1", missing, "LEFT"), b"QUEUED")
        self.assertEqual(self.cmd("EXEC"), [None])

    def test_zmpop_and_bzmpop(self):
        zkey = self.key("zmpop:zset")
        zkey2 = self.key("zmpop:other")
        missing = self.key("zmpop:missing")
        plain = self.key("zmpop:string")
        self.cmd("DEL", zkey, zkey2, missing, plain)
        self.cmd("ZADD", zkey, "1", "a", "2", "b", "3", "c")

        # ZMPOP replies with [key, [[member, score], ...]].
        self.assertEqual(self.cmd("ZMPOP", "2", missing, zkey, "MIN", "COUNT", "2"),
                         [zkey, [[b"a", b"1"], [b"b", b"2"]]])
        self.assertEqual(self.cmd("ZRANGE", zkey, "0", "-1", "WITHSCORES"),
                         [b"c", b"3"])
        self.assertEqual(self.cmd("ZMPOP", "1", zkey, "MAX", "COUNT", "5"),
                         [zkey, [[b"c", b"3"]]])
        self.assertEqual(self.cmd("EXISTS", zkey), 0)
        self.assertIsNone(self.cmd("ZMPOP", "1", missing, "MIN"))
        self.assertIsNone(self.cmd("ZMPOP", "2", missing, zkey, "MIN"))

        with self.assertRaises(RedisError):
            self.cmd("ZMPOP", "0", zkey, "MIN")
        with self.assertRaises(RedisError):
            self.cmd("ZMPOP", "99999", zkey, "MIN")
        with self.assertRaises(RedisError):
            self.cmd("ZMPOP", "9223372036854775807", zkey, "MIN")
        with self.assertRaises(RedisError):
            self.cmd("ZMPOP", "1", zkey, "MIDDLE")
        with self.assertRaises(RedisError):
            self.cmd("ZMPOP", "1", zkey, "MIN", "COUNT", "0")
        with self.assertRaises(RedisError):
            self.cmd("ZMPOP", "1", zkey, "MIN", "LIMIT", "1")
        with self.assertRaises(RedisError):
            self.cmd("ZMPOP", "1", zkey, "MIN", "COUNT", "1", "COUNT", "2")
        self.cmd("SET", plain, "x")
        with self.assertRaises(RedisError):
            self.cmd("ZMPOP", "1", plain, "MIN")

        # ZPOPMIN/ZPOPMAX share the parser and validate COUNT.
        self.cmd("ZADD", zkey, "1", "a", "2", "b", "3", "c")
        self.assertEqual(self.cmd("ZPOPMIN", zkey, "0"), [])
        self.assertEqual(self.cmd("ZPOPMAX", zkey, "2"), [b"c", b"3", b"b", b"2"])
        with self.assertRaises(RedisError):
            self.cmd("ZPOPMIN", zkey, "-1")
        self.assertEqual(self.cmd("ZPOPMIN", zkey), [b"a", b"1"])
        self.assertEqual(self.cmd("EXISTS", zkey), 0)

        # BZMPOP serves an already non-empty zset immediately.
        self.cmd("ZADD", zkey2, "1", "a", "2", "b")
        self.assertEqual(self.cmd("BZMPOP", "0", "1", zkey2, "MAX", "COUNT", "1"),
                         [zkey2, [[b"b", b"2"]]])

        # BZMPOP waits for a ZADD on any of its keys.
        bzkey = self.key("zmpop:block")
        self.cmd("DEL", bzkey)
        blocked = self.connect()
        try:
            blocked.sock.sendall(blocked.encode(
                ("BZMPOP", "5", "2", missing, bzkey, "MIN", "COUNT", "2")))
            time.sleep(0.2)
            self.cmd("ZADD", bzkey, "1", "a", "3", "c", "4", "d")
            self.assertEqual(blocked._read(), [bzkey, [[b"a", b"1"], [b"c", b"3"]]])
        finally:
            blocked.close()
        self.assertEqual(self.cmd("ZRANGE", bzkey, "0", "-1", "WITHSCORES"),
                         [b"d", b"4"])

        # A blocking pop that empties the zset deletes the key.
        blocked = self.connect()
        try:
            blocked.sock.sendall(blocked.encode(
                ("BZMPOP", "5", "1", bzkey, "MAX", "COUNT", "5")))
            time.sleep(0.2)
            self.assertEqual(blocked._read(), [bzkey, [[b"d", b"4"]]])
        finally:
            blocked.close()
        self.assertEqual(self.cmd("EXISTS", bzkey), 0)

        self.assertIsNone(self.cmd("BZMPOP", "0.1", "1", missing, "MIN"))
        self.assertEqual(self.cmd("MULTI"), b"OK")
        self.assertEqual(self.cmd("BZMPOP", "0", "1", missing, "MIN"), b"QUEUED")
        self.assertEqual(self.cmd("EXEC"), [None])

    def test_client_no_touch_and_no_evict(self):
        key = self.key("client:flags")
        self.cmd("DEL", key)

        self.assertEqual(self.cmd("CLIENT", "NO-EVICT", "on"), b"OK")
        self.assertIn(b"flags=e", self.cmd("CLIENT", "LIST"))
        self.assertEqual(self.cmd("CLIENT", "NO-EVICT", "off"), b"OK")
        with self.assertRaises(RedisError):
            self.cmd("CLIENT", "NO-EVICT", "maybe")
        with self.assertRaises(RedisError):
            self.cmd("CLIENT", "NO-EVICT")

        # CLIENT NO-TOUCH suppresses the LRU update, except for TOUCH itself.
        self.cmd("SET", key, "v")
        time.sleep(1.1)
        self.assertGreaterEqual(self.cmd("OBJECT", "IDLETIME", key), 1)
        self.assertEqual(self.cmd("CLIENT", "NO-TOUCH", "on"), b"OK")
        self.assertIn(b"flags=T", self.cmd("CLIENT", "LIST"))
        self.assertEqual(self.cmd("GET", key), b"v")
        time.sleep(1.1)
        self.assertGreaterEqual(self.cmd("OBJECT", "IDLETIME", key), 2)
        self.assertEqual(self.cmd("TOUCH", key), 1)
        self.assertLessEqual(self.cmd("OBJECT", "IDLETIME", key), 1)
        self.assertEqual(self.cmd("CLIENT", "NO-TOUCH", "off"), b"OK")
        self.assertEqual(self.cmd("GET", key), b"v")
        self.assertLessEqual(self.cmd("OBJECT", "IDLETIME", key), 1)
        with self.assertRaises(RedisError):
            self.cmd("CLIENT", "NO-TOUCH", "maybe")

        # RESET clears both flags.
        self.assertEqual(self.cmd("CLIENT", "NO-EVICT", "on"), b"OK")
        self.assertEqual(self.cmd("RESET"), b"OK")
        self.assertNotIn(b"flags=e", self.cmd("CLIENT", "LIST"))

    def test_stream_xtrim_minid_and_limit(self):
        key = self.key("xtrim:minid")
        for ms in range(1, 7):
            self.assertEqual(self.cmd("XADD", key, "%d-1" % ms, "f", ms),
                             ("%d-1" % ms).encode())

        self.assertEqual(self.cmd("XTRIM", key, "MINID", "3-1"), 2)
        self.assertEqual(self.cmd("XLEN", key), 4)
        self.assertEqual(self.cmd("XRANGE", key, "-", "+")[0],
                         [b"3-1", [b"f", b"3"]])

        # '=' is accepted as the explicit form of MINID.
        self.assertEqual(self.cmd("XTRIM", key, "MINID", "=", "6-1"), 3)
        self.assertEqual(self.cmd("XLEN", key), 1)

        # Approximate trimming only removes whole radix tree nodes.
        self.assertEqual(self.cmd("XTRIM", key, "MINID", "~", "6-1"), 0)
        self.assertEqual(self.cmd("XLEN", key), 1)
        self.assertEqual(self.cmd("XTRIM", key, "MINID", "~", "7-1"), 1)
        self.assertEqual(self.cmd("XLEN", key), 0)
        self.assertEqual(self.cmd("XRANGE", key, "-", "+"), [])
        self.assertEqual(self.flat_map(self.cmd("XINFO", "STREAM", key))
                         [b"recorded-first-entry-id"], b"0-0")

        # Exact MINID trimming crosses listpack nodes (100 entries each).
        key = self.key("xtrim:minid:multi")
        for ms in range(1, 201):
            self.cmd("XADD", key, "%d-1" % ms, "f", ms)
        self.assertEqual(self.cmd("XTRIM", key, "MINID", "150-1"), 149)
        self.assertEqual(self.cmd("XLEN", key), 51)
        self.assertEqual(self.cmd("XRANGE", key, "-", "+", "COUNT", 1)[0][0],
                         b"150-1")

        # LIMIT is only valid together with the approximate form.
        with self.assertRaises(RedisError):
            self.cmd("XTRIM", key, "MAXLEN", "1", "LIMIT", "10")
        with self.assertRaises(RedisError):
            self.cmd("XTRIM", key, "MAXLEN", "~", "1", "LIMIT", "-1")
        with self.assertRaises(RedisError):
            self.cmd("XTRIM", key, "MAXLEN", "1", "MINID", "2-2")
        with self.assertRaises(RedisError):
            self.cmd("XTRIM", key)
        self.assertEqual(self.cmd("XTRIM", self.key("xtrim:missing"),
                                  "MINID", "1-1"), 0)

        # XADD accepts the same trimming options, including LIMIT.
        key = self.key("xadd:minid")
        for ms in range(1, 6):
            self.cmd("XADD", key, "MINID", "3-1", "%d-1" % ms, "f", ms)
        self.assertEqual(self.cmd("XLEN", key), 3)

        key = self.key("xadd:minid:limit")
        for ms in range(1, 201):
            self.cmd("XADD", key, "%d-1" % ms, "f", ms)
        self.assertEqual(self.cmd("XADD", key, "MINID", "~", "150-1",
                                  "LIMIT", "100", "201-1", "f", 201), b"201-1")
        self.assertEqual(self.cmd("XLEN", key), 101)

        with self.assertRaises(RedisError):
            self.cmd("XADD", key, "LIMIT", "10", "*", "f", "v")
        with self.assertRaises(RedisError):
            self.cmd("XADD", key, "MAXLEN", "5", "LIMIT", "10", "*", "f", "v")
        with self.assertRaises(RedisError):
            self.cmd("XADD", key, "MAXLEN", "5", "MINID", "1-1", "*", "f", "v")

    def test_stream_xadd_nomkstream(self):
        missing = self.key("xadd:nomk")
        self.cmd("DEL", missing)
        self.assertIsNone(self.cmd("XADD", missing, "NOMKSTREAM", "*", "f", "v"))
        self.assertEqual(self.cmd("EXISTS", missing), 0)
        self.assertIsNone(self.cmd("XADD", missing, "MAXLEN", "5",
                                   "NOMKSTREAM", "*", "f", "v"))
        self.assertEqual(self.cmd("EXISTS", missing), 0)

        key = self.key("xadd:nomk:exists")
        self.assertEqual(self.cmd("XADD", key, "1-1", "f", "v"), b"1-1")
        self.assertEqual(self.cmd("XADD", key, "NOMKSTREAM", "2-1", "f", "v"),
                         b"2-1")
        self.assertEqual(self.cmd("XADD", key, "MAXLEN", "10", "NOMKSTREAM",
                                  "3-1", "f", "v"), b"3-1")
        self.assertEqual(self.cmd("XLEN", key), 3)

        string = self.key("xadd:nomk:str")
        self.cmd("SET", string, "x")
        with self.assertRaises(RedisError):
            self.cmd("XADD", string, "NOMKSTREAM", "*", "f", "v")

    def test_stream_xsetid_options(self):
        key = self.key("xsetid")
        self.cmd("XADD", key, "1-1", "f", "v")
        self.assertEqual(self.cmd("XSETID", key, "5-5"), b"OK")
        info = self.flat_map(self.cmd("XINFO", "STREAM", key))
        self.assertEqual(info[b"last-generated-id"], b"5-5")
        self.assertEqual(info[b"entries-added"], 1)
        self.assertEqual(info[b"max-deleted-entry-id"], b"0-0")

        self.assertEqual(self.cmd("XSETID", key, "9-9", "ENTRIESADDED", "100",
                                  "MAXDELETEDID", "8-8"), b"OK")
        info = self.flat_map(self.cmd("XINFO", "STREAM", key))
        self.assertEqual(info[b"last-generated-id"], b"9-9")
        self.assertEqual(info[b"entries-added"], 100)
        self.assertEqual(info[b"max-deleted-entry-id"], b"8-8")

        for args in (("0-9",),
                     ("9-9", "ENTRIESADDED", "0"),
                     ("9-9", "ENTRIESADDED", "-1"),
                     ("9-9", "MAXDELETEDID", "10-10"),
                     ("9-9", "BOGUS", "1"),
                     ("not-an-id",)):
            with self.assertRaises(RedisError):
                self.cmd("XSETID", key, *args)
        info = self.flat_map(self.cmd("XINFO", "STREAM", key))
        self.assertEqual(info[b"last-generated-id"], b"9-9")
        self.assertEqual(info[b"entries-added"], 100)
        self.assertEqual(info[b"max-deleted-entry-id"], b"8-8")

        with self.assertRaises(RedisError):
            self.cmd("XSETID", self.key("xsetid:missing"), "1-1")

    def test_stream_xgroup_createconsumer_and_pending_idle(self):
        key = self.key("xgroup:consumer")
        self.cmd("XADD", key, "1-1", "f", "v")
        self.cmd("XADD", key, "2-1", "f", "v")
        self.assertEqual(self.cmd("XGROUP", "CREATE", key, "g", "0"), b"OK")

        self.assertEqual(self.cmd("XGROUP", "CREATECONSUMER", key, "g", "c1"), 1)
        self.assertEqual(self.cmd("XGROUP", "CREATECONSUMER", key, "g", "c1"), 0)
        consumers = self.cmd("XINFO", "CONSUMERS", key, "g")
        self.assertEqual([self.flat_map(c)[b"name"] for c in consumers], [b"c1"])

        with self.assertRaises(RedisError):
            self.cmd("XGROUP", "CREATECONSUMER", key, "missing", "c1")
        with self.assertRaises(RedisError):
            self.cmd("XGROUP", "CREATECONSUMER", self.key("xgroup:missing"),
                     "g", "c1")

        read = self.cmd("XREADGROUP", "GROUP", "g", "c2", "COUNT", "2",
                        "STREAMS", key, ">")
        self.assertEqual(len(read[0][1]), 2)

        # Backdate one pending entry so the IDLE filter can tell them apart.
        old = self.now_ms() - 60000
        self.assertEqual(self.cmd("XCLAIM", key, "g", "c2", "0", "1-1",
                                  "TIME", old, "JUSTID"), [b"1-1"])

        pending = self.cmd("XPENDING", key, "g", "IDLE", "30000", "-", "+", "10")
        self.assertEqual([row[0] for row in pending], [b"1-1"])
        pending = self.cmd("XPENDING", key, "g", "IDLE", "0", "-", "+", "10")
        self.assertEqual([row[0] for row in pending], [b"1-1", b"2-1"])
        pending = self.cmd("XPENDING", key, "g", "IDLE", "30000",
                           "-", "+", "10", "c2")
        self.assertEqual([row[0] for row in pending], [b"1-1"])
        self.assertEqual(self.cmd("XPENDING", key, "g", "IDLE", "30000",
                                  "-", "+", "10", "c1"), [])

        with self.assertRaises(RedisError):
            self.cmd("XPENDING", key, "g", "IDLE", "0", "-", "+")
        with self.assertRaises(RedisError):
            self.cmd("XPENDING", key, "g", "IDLE", "not-a-number",
                     "-", "+", "10")

    def test_stream_xautoclaim(self):
        key = self.key("xautoclaim")
        for ms in range(1, 6):
            self.cmd("XADD", key, "%d-1" % ms, "f", ms)
        self.assertEqual(self.cmd("XGROUP", "CREATE", key, "g", "0"), b"OK")
        self.assertEqual(len(self.cmd("XREADGROUP", "GROUP", "g", "c1",
                                      "COUNT", "5", "STREAMS", key, ">")[0][1]),
                         5)

        reply = self.cmd("XAUTOCLAIM", key, "g", "c2", "0", "0-0", "COUNT", "2")
        self.assertEqual(len(reply), 3)
        self.assertEqual(reply[0], b"3-1")
        self.assertEqual([entry[0] for entry in reply[1]], [b"1-1", b"2-1"])
        self.assertEqual(reply[1][0], [b"1-1", [b"f", b"1"]])
        self.assertEqual(reply[2], [])

        reply = self.cmd("XAUTOCLAIM", key, "g", "c2", "0", "3-1", "COUNT", "10")
        self.assertEqual(reply[0], b"0-0")
        self.assertEqual([entry[0] for entry in reply[1]],
                         [b"3-1", b"4-1", b"5-1"])
        pending = self.cmd("XPENDING", key, "g", "-", "+", "10")
        self.assertEqual({row[1] for row in pending}, {b"c2"})

        # JUSTID returns IDs only and does not bump the delivery counter.
        before = {row[0]: row[3] for row in
                  self.cmd("XPENDING", key, "g", "-", "+", "10")}
        reply = self.cmd("XAUTOCLAIM", key, "g", "c3", "0", "0-0", "JUSTID")
        self.assertEqual(reply[0], b"0-0")
        self.assertEqual(reply[1], [b"1-1", b"2-1", b"3-1", b"4-1", b"5-1"])
        self.assertEqual(reply[2], [])
        after = {row[0]: row[3] for row in
                 self.cmd("XPENDING", key, "g", "-", "+", "10")}
        self.assertEqual(after, before)
        self.assertEqual({row[1] for row in
                          self.cmd("XPENDING", key, "g", "-", "+", "10")}, {b"c3"})

        # PEL entries whose stream entry was deleted are dropped and reported.
        self.assertEqual(self.cmd("XDEL", key, "1-1"), 1)
        reply = self.cmd("XAUTOCLAIM", key, "g", "c4", "0", "0-0", "COUNT", "10")
        self.assertEqual(reply[0], b"0-0")
        self.assertEqual(reply[2], [b"1-1"])
        pending = self.cmd("XPENDING", key, "g", "-", "+", "10")
        self.assertNotIn(b"1-1", [row[0] for row in pending])
        self.assertEqual({row[1] for row in pending}, {b"c4"})

        # An exclusive start ID is honored.
        reply = self.cmd("XAUTOCLAIM", key, "g", "c5", "0", "(2-1", "JUSTID")
        self.assertEqual(reply[1], [b"3-1", b"4-1", b"5-1"])

        for args in (("COUNT", "0"), ("COUNT", "not-a-number")):
            with self.assertRaises(RedisError):
                self.cmd("XAUTOCLAIM", key, "g", "c5", "0", "0-0", *args)
        with self.assertRaises(RedisError):
            self.cmd("XAUTOCLAIM", key, "g", "c5", "0", "bad-id")
        with self.assertRaises(RedisError):
            self.cmd("XAUTOCLAIM", key, "g", "c5", "0",
                     "(18446744073709551615-18446744073709551615")
        with self.assertRaises(RedisError):
            self.cmd("XAUTOCLAIM", self.key("xautoclaim:missing"), "g", "c",
                     "0", "0-0")
        with self.assertRaises(RedisError):
            self.cmd("XAUTOCLAIM", key, "missing", "c", "0", "0-0")

    def test_stream_xinfo_full(self):
        key = self.key("xinfo:full")
        for ms in range(1, 6):
            self.cmd("XADD", key, "%d-1" % ms, "f", ms)
        self.cmd("XGROUP", "CREATE", key, "g", "0")
        self.cmd("XREADGROUP", "GROUP", "g", "c1", "COUNT", "3",
                 "STREAMS", key, ">")
        self.assertEqual(self.cmd("XDEL", key, "1-1"), 1)

        info = self.flat_map(self.cmd("XINFO", "STREAM", key))
        self.assertEqual(info[b"length"], 4)
        self.assertEqual(info[b"entries-added"], 5)
        self.assertEqual(info[b"max-deleted-entry-id"], b"1-1")
        self.assertEqual(info[b"recorded-first-entry-id"], b"2-1")
        self.assertEqual(info[b"groups"], 1)
        self.assertEqual(info[b"first-entry"], [b"2-1", [b"f", b"2"]])
        self.assertEqual(info[b"last-entry"], [b"5-1", [b"f", b"5"]])
        self.assertNotIn(b"entries", info)

        full = self.flat_map(self.cmd("XINFO", "STREAM", key, "FULL", "COUNT", "2"))
        self.assertEqual([entry[0] for entry in full[b"entries"]],
                         [b"2-1", b"3-1"])
        self.assertNotIn(b"first-entry", full)
        self.assertEqual(len(full[b"groups"]), 1)
        group = self.flat_map(full[b"groups"][0])
        self.assertEqual(group[b"name"], b"g")
        self.assertEqual(group[b"pel-count"], 3)
        # COUNT also caps the pending entries lists (1-1 is still pending
        # even though its stream entry was deleted).
        self.assertEqual([row[0] for row in group[b"pending"]],
                         [b"1-1", b"2-1"])
        consumers = [self.flat_map(consumer) for consumer in group[b"consumers"]]
        self.assertEqual([c[b"name"] for c in consumers], [b"c1"])
        self.assertEqual(consumers[0][b"pel-count"], 3)
        self.assertEqual(len(consumers[0][b"pending"]), 2)

        # COUNT 0 means no limit, a negative COUNT falls back to the default.
        full = self.flat_map(self.cmd("XINFO", "STREAM", key, "FULL", "COUNT", "0"))
        self.assertEqual(len(full[b"entries"]), 4)
        full = self.flat_map(self.cmd("XINFO", "STREAM", key, "FULL", "COUNT", "-1"))
        self.assertEqual(len(full[b"entries"]), 4)

        with self.assertRaises(RedisError):
            self.cmd("XINFO", "STREAM", key, "BOGUS")
        with self.assertRaises(RedisError):
            self.cmd("XINFO", "STREAM", key, "FULL", "COUNT")

    def test_stream_aof_noack_consumer(self):
        if self.server is None:
            self.skipTest("AOF replay test requires --server")
        key = self.key("stream:aof:noack")
        self.cmd("XADD", key, "1-1", "f", "v")
        self.assertEqual(self.cmd("XGROUP", "CREATE", key, "g", "0"), b"OK")
        self.assertEqual(len(self.cmd("XREADGROUP", "GROUP", "g", "c1",
                                      "NOACK", "STREAMS", key, ">")[0][1]), 1)
        self.assertEqual(self.cmd("XPENDING", key, "g"), [0, None, None, None])
        self.assertEqual([self.flat_map(consumer)[b"name"] for consumer in
                          self.cmd("XINFO", "CONSUMERS", key, "g")], [b"c1"])

        # The consumer has no PEL entry, so only a propagated
        # XGROUP CREATECONSUMER can recreate it when the AOF is replayed.
        self.restart()
        self.assertEqual([self.flat_map(consumer)[b"name"] for consumer in
                          self.cmd("XINFO", "CONSUMERS", key, "g")], [b"c1"])

    def test_stream_aof_rewrite_counters(self):
        if self.server is None:
            self.skipTest("AOF rewrite test requires --server")
        key = self.key("stream:aof:rewrite")
        for ms in range(1, 4):
            self.cmd("XADD", key, "%d-1" % ms, "f", ms)
        self.assertEqual(self.cmd("XDEL", key, "1-1"), 1)
        self.assertEqual(self.cmd("XSETID", key, "9-9", "ENTRIESADDED", "42",
                                  "MAXDELETEDID", "1-1"), b"OK")

        self.assertEqual(self.cmd("BGREWRITEAOF"), b"Background append only file rewriting started")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            info = self.cmd("INFO", "persistence")
            if (b"aof_rewrite_in_progress:0" in info and
                    b"aof_rewrite_scheduled:0" in info):
                break
            time.sleep(0.05)
        else:
            self.fail("AOF rewrite did not finish")

        self.restart()
        info = self.flat_map(self.cmd("XINFO", "STREAM", key))
        self.assertEqual(info[b"length"], 2)
        self.assertEqual(info[b"last-generated-id"], b"9-9")
        self.assertEqual(info[b"entries-added"], 42)
        self.assertEqual(info[b"max-deleted-entry-id"], b"1-1")


    def test_bitcount_bitpos_bit_ranges(self):
        key = self.key("bits")
        self.cmd("SET", key, b"\xff\x00\x0f")
        self.assertEqual(self.cmd("BITCOUNT", key), 12)
        self.assertEqual(self.cmd("BITCOUNT", key, "0", "0"), 8)
        self.assertEqual(self.cmd("BITCOUNT", key, "0", "0", "BIT"), 1)
        self.assertEqual(self.cmd("BITCOUNT", key, "0", "7", "BIT"), 8)
        self.assertEqual(self.cmd("BITCOUNT", key, "0", "-1", "BIT"), 12)
        self.assertEqual(self.cmd("BITCOUNT", key, "8", "15", "BIT"), 0)
        self.assertEqual(self.cmd("BITCOUNT", key, "16", "23", "BIT"), 4)
        self.assertEqual(self.cmd("BITCOUNT", key, "0", "0", "BYTE"), 8)
        with self.assertRaises(RedisError):
            self.cmd("BITCOUNT", key, "0", "0", "NIBBLE")

        self.assertEqual(self.cmd("BITPOS", key, "0"), 8)
        self.assertEqual(self.cmd("BITPOS", key, "0", "0", "-1", "BIT"), 8)
        self.assertEqual(self.cmd("BITPOS", key, "1", "0", "-1", "BIT"), 0)
        self.assertEqual(self.cmd("BITPOS", key, "1", "8", "-1", "BIT"), 20)
        self.assertEqual(self.cmd("BITPOS", key, "0", "2", "-1", "BIT"), 8)
        self.assertEqual(self.cmd("BITPOS", key, "0", "2", "-1"), 16)

        # A string of all ones has no zero bit.
        ones = self.key("ones")
        self.cmd("SET", ones, b"\xff")
        # Without an explicit range the first bit past the end is reported.
        self.assertEqual(self.cmd("BITPOS", ones, "0"), 8)
        self.assertEqual(self.cmd("BITPOS", ones, "0", "0", "-1", "BIT"), -1)
        self.assertEqual(self.cmd("BITPOS", ones, "1"), 0)
        with self.assertRaises(RedisError):
            self.cmd("BITPOS", key, "0", "0", "-1", "BITS")

    def test_bitfield_ro(self):
        key = self.key("bitfield")
        self.cmd("SET", key, b"\x01\x02\x03\x04")
        self.assertEqual(self.cmd("BITFIELD_RO", key, "GET", "u8", "0",
                                  "GET", "u8", "8"), [1, 2])
        self.assertEqual(self.cmd("BITFIELD_RO", key, "GET", "u16", "0"), [258])
        with self.assertRaises(RedisError):
            self.cmd("BITFIELD_RO", key, "SET", "u8", "0", "9")
        with self.assertRaises(RedisError):
            self.cmd("BITFIELD_RO", key, "INCRBY", "u8", "0", "1")
        # The failed SET must not have modified the value.
        self.assertEqual(self.cmd("GET", key), b"\x01\x02\x03\x04")
        # The regular BITFIELD still writes.
        self.assertEqual(self.cmd("BITFIELD", key, "SET", "u8", "0", "9"), [1])
        self.assertEqual(self.cmd("GET", key), b"\x09\x02\x03\x04")

    def test_lcs(self):
        a = self.key("lcs:a")
        b = self.key("lcs:b")
        self.cmd("SET", a, "ohmytext")
        self.cmd("SET", b, "mynewtext")
        self.assertEqual(self.cmd("LCS", a, b), b"mytext")
        self.assertEqual(self.cmd("LCS", a, b, "LEN"), 6)
        self.assertEqual(self.cmd("LCS", a, b, "IDX"), [
            b"matches", [[[4, 7], [5, 8]], [[2, 3], [0, 1]]],
            b"len", 6,
        ])
        self.assertEqual(self.cmd("LCS", a, b, "IDX", "WITHMATCHLEN"), [
            b"matches", [[[4, 7], [5, 8], 4], [[2, 3], [0, 1], 2]],
            b"len", 6,
        ])
        self.assertEqual(self.cmd("LCS", a, b, "IDX", "MINMATCHLEN", "5"), [
            b"matches", [], b"len", 6,
        ])
        with self.assertRaises(RedisError):
            self.cmd("LCS", a, b, "LEN", "IDX")
        with self.assertRaises(RedisError):
            self.cmd("LCS", a, b, "NOSUCHOPTION")

        missing = self.key("lcs:missing")
        self.assertEqual(self.cmd("LCS", missing, b), b"")
        self.assertEqual(self.cmd("LCS", missing, b, "LEN"), 0)
        self.assertEqual(self.cmd("LCS", missing, b, "IDX"),
                         [b"matches", [], b"len", 0])

        wrong = self.key("lcs:list")
        self.cmd("RPUSH", wrong, "x")
        with self.assertRaises(RedisError):
            self.cmd("LCS", wrong, b)

    def test_sort_ro(self):
        key = self.key("sortro")
        self.cmd("RPUSH", key, "3", "1", "2")
        self.assertEqual(self.cmd("SORT_RO", key), [b"1", b"2", b"3"])
        self.assertEqual(self.cmd("SORT_RO", key, "DESC"), [b"3", b"2", b"1"])
        self.assertEqual(self.cmd("SORT_RO", key, "LIMIT", "0", "2"),
                         [b"1", b"2"])
        out = self.key("sortro:out")
        with self.assertRaises(RedisError):
            self.cmd("SORT_RO", key, "STORE", out)
        self.assertEqual(self.cmd("EXISTS", out), 0)
        self.assertEqual(self.cmd("SORT", key, "STORE", out), 3)
        self.assertEqual(self.cmd("LRANGE", out, "0", "-1"),
                         [b"1", b"2", b"3"])

    def test_eval_ro(self):
        key = self.key("evalro")
        self.cmd("SET", key, "value")
        self.assertEqual(self.cmd("EVAL_RO",
                                  "return redis.call('get',KEYS[1])", "1", key),
                         b"value")
        with self.assertRaises(RedisError):
            self.cmd("EVAL_RO",
                     "return redis.call('set',KEYS[1],'changed')", "1", key)
        self.assertEqual(self.cmd("GET", key), b"value")

        sha = self.cmd("SCRIPT", "LOAD", "return redis.call('get',KEYS[1])")
        self.assertEqual(self.cmd("EVALSHA_RO", sha, "1", key), b"value")
        sha_write = self.cmd("SCRIPT", "LOAD",
                             "return redis.call('set',KEYS[1],'changed')")
        with self.assertRaises(RedisError):
            self.cmd("EVALSHA_RO", sha_write, "1", key)
        self.assertEqual(self.cmd("GET", key), b"value")

        # A normal EVAL can still write.
        self.assertEqual(self.cmd("EVAL",
                                  "return redis.call('set',KEYS[1],'new')",
                                  "1", key), b"OK")
        self.assertEqual(self.cmd("GET", key), b"new")

    def test_client_info_unpause_pause(self):
        if self.server is None:
            self.skipTest("CLIENT PAUSE tests require --server")
        info = self.cmd("CLIENT", "INFO")
        self.assertIn(b"id=", info)
        self.assertIn(b"addr=", info)
        self.assertIn(b"name=", info)

        with self.connect() as other:
            self.assertEqual(self.cmd("CLIENT", "PAUSE", "5000", "WRITE"),
                             b"OK")
            started = time.monotonic()
            self.assertEqual(other.command("GET", self.key("pause:missing")),
                             None)
            self.assertLess(time.monotonic() - started, 2.0)

            pool = ThreadPoolExecutor(max_workers=1)
            self.addCleanup(pool.shutdown)
            pending = pool.submit(other.command, "SET",
                                  self.key("pause:set"), "1")
            time.sleep(0.3)
            self.assertFalse(pending.done())
            self.assertEqual(self.cmd("CLIENT", "UNPAUSE"), b"OK")
            self.assertEqual(pending.result(timeout=5), b"OK")
            self.assertEqual(self.cmd("GET", self.key("pause:set")), b"1")

            # ALL pauses reads too, but the pause times out on its own.
            self.assertEqual(self.cmd("CLIENT", "PAUSE", "300", "ALL"), b"OK")
            started = time.monotonic()
            self.assertEqual(other.command("PING"), b"PONG")
            self.assertGreaterEqual(time.monotonic() - started, 0.2)

        with self.assertRaises(RedisError):
            self.cmd("CLIENT", "PAUSE", "100", "SOMETHING")

    def test_config_multi_get_set(self):
        if self.server is None:
            self.skipTest("CONFIG tests require --server")
        self.assertEqual(self.cmd("CONFIG", "GET", "maxmemory",
                                  "maxmemory-samples"), [
            b"maxmemory", b"0",
            b"maxmemory-samples", b"5",
        ])
        # Overlapping patterns must not repeat a parameter.
        self.assertEqual(self.cmd("CONFIG", "GET", "maxmemory", "maxmemory"),
                         [b"maxmemory", b"0"])
        self.assertEqual(self.cmd("CONFIG", "GET", "maxmemory*"),
                         self.cmd("CONFIG", "GET", "maxmemory*"))

        self.assertEqual(self.cmd("CONFIG", "SET", "maxmemory-samples", "7",
                                  "timeout", "123"), b"OK")
        self.assertEqual(self.cmd("CONFIG", "GET", "maxmemory-samples",
                                  "timeout"),
                         [b"maxmemory-samples", b"7", b"timeout", b"123"])

        # Pairs are applied in order: the first one stays applied when a
        # later one is rejected.
        with self.assertRaises(RedisError):
            self.cmd("CONFIG", "SET", "maxmemory-samples", "9",
                     "no-such-parameter", "1")
        self.assertEqual(self.cmd("CONFIG", "GET", "maxmemory-samples"),
                         [b"maxmemory-samples", b"9"])
        with self.assertRaises(RedisError):
            self.cmd("CONFIG", "SET", "maxmemory-samples", "1", "timeout")
        self.cmd("CONFIG", "SET", "maxmemory-samples", "5", "timeout", "0")

    def test_command_list_and_getkeysandflags(self):
        names = self.cmd("COMMAND", "LIST")
        self.assertIn(b"get", names)
        self.assertIn(b"bitfield_ro", names)

        bits = self.cmd("COMMAND", "LIST", "FILTERBY", "PATTERN", "bit*")
        self.assertIn(b"bitcount", bits)
        self.assertIn(b"bitfield_ro", bits)
        self.assertTrue(all(name.startswith(b"bit") for name in bits))

        reads = self.cmd("COMMAND", "LIST", "FILTERBY", "ACLCAT", "read")
        self.assertIn(b"get", reads)
        self.assertNotIn(b"set", reads)
        writes = self.cmd("COMMAND", "LIST", "FILTERBY", "ACLCAT", "write")
        self.assertIn(b"set", writes)
        self.assertNotIn(b"get", writes)
        self.assertEqual(self.cmd("COMMAND", "LIST", "FILTERBY", "MODULE",
                                  "whatever"), [])
        with self.assertRaises(RedisError):
            self.cmd("COMMAND", "LIST", "FILTERBY", "NOPE", "x")

        self.assertEqual(self.cmd("COMMAND", "GETKEYSANDFLAGS", "SET",
                                  "key", "value"),
                         [[b"key", [b"RW", b"access", b"update"]]])
        self.assertEqual(self.cmd("COMMAND", "GETKEYSANDFLAGS", "GET", "key"),
                         [[b"key", [b"RO", b"access"]]])
        self.assertEqual(self.cmd("COMMAND", "GETKEYSANDFLAGS", "MSET",
                                  "a", "1", "b", "2"),
                         [[b"a", [b"RW", b"access", b"update"]],
                          [b"b", [b"RW", b"access", b"update"]]])
        with self.assertRaises(RedisError):
            self.cmd("COMMAND", "GETKEYSANDFLAGS", "NOSUCHCOMMAND", "x")
        with self.assertRaises(RedisError):
            self.cmd("COMMAND", "GETKEYSANDFLAGS", "PING")

        docs = self.flat_map(self.cmd("COMMAND", "DOCS", "get", "zrank",
                                      "nosuchcommand"))
        self.assertIn(b"get", docs)
        get_docs = self.flat_map(docs[b"get"])
        self.assertEqual(get_docs[b"group"], b"string")
        self.assertIn(b"value", get_docs[b"summary"].lower())
        self.assertEqual(get_docs[b"since"], b"1.0.0")
        self.assertEqual(get_docs[b"arguments"], b"key")
        zrank_docs = self.flat_map(docs[b"zrank"])
        self.assertEqual(zrank_docs[b"group"], b"sorted_set")
        self.assertIn(b"WITHSCORE", zrank_docs[b"arguments"])
        self.assertNotIn(b"nosuchcommand", docs)

        help_text = self.cmd("COMMAND", "HELP")
        self.assertTrue(any(b"DOCS" in line for line in help_text))

    def test_zrank_withscore(self):
        key = self.key("zrank:ws")
        missing = self.key("zrank:missing")
        plain = self.key("zrank:string")
        self.cmd("DEL", key, missing, plain)
        self.cmd("ZADD", key, "1", "a", "2", "b", "3", "c")

        self.assertEqual(self.cmd("ZRANK", key, "a"), 0)
        self.assertEqual(self.cmd("ZREVRANK", key, "a"), 2)
        self.assertEqual(self.cmd("ZRANK", key, "b", "WITHSCORE"),
                         [1, b"2"])
        self.assertEqual(self.cmd("ZREVRANK", key, "b", "WITHSCORE"),
                         [1, b"2"])
        self.assertIsNone(self.cmd("ZRANK", key, "missing"))
        self.assertIsNone(self.cmd("ZRANK", key, "missing", "WITHSCORE"))
        self.assertIsNone(self.cmd("ZRANK", missing, "a"))
        self.assertIsNone(self.cmd("ZRANK", missing, "a", "WITHSCORE"))

        self.cmd("SET", plain, "x")
        with self.assertRaises(RedisError):
            self.cmd("ZRANK", plain, "a")
        with self.assertRaises(RedisError):
            self.cmd("ZRANK", key, "a", "WITHSCORES")
        with self.assertRaises(RedisError):
            self.cmd("ZRANK", key, "a", "WITHSCORE", "extra")

    def test_geosearch_geosearchstore(self):
        g = self.key("geo:search")
        self.assertEqual(self.cmd("GEOADD", g,
                                  "13.361389", "38.115556", "palermo",
                                  "15.087269", "37.502669", "catania",
                                  "12.5", "41.9", "rome"), 3)

        self.assertEqual(self.cmd("GEOSEARCH", g, "FROMLONLAT", "15", "37",
                                  "BYRADIUS", "200", "km", "ASC"),
                         [b"catania", b"palermo"])
        self.assertEqual(self.cmd("GEOSEARCH", g, "FROMMEMBER", "palermo",
                                  "BYRADIUS", "200", "km", "ASC"),
                         [b"palermo", b"catania"])
        self.assertEqual(self.cmd("GEOSEARCH", g, "FROMMEMBER", "palermo",
                                  "BYBOX", "400", "400", "km", "ASC",
                                  "COUNT", "1"), [b"palermo"])
        self.assertEqual(self.cmd("GEOSEARCH", g, "FROMLONLAT", "15", "37",
                                  "BYRADIUS", "200", "km", "DESC"),
                         [b"palermo", b"catania"])

        rows = self.cmd("GEOSEARCH", g, "FROMLONLAT", "15", "37",
                        "BYRADIUS", "200", "km", "ASC",
                        "WITHCOORD", "WITHDIST", "WITHHASH")
        self.assertEqual([row[0] for row in rows], [b"catania", b"palermo"])
        self.assertAlmostEqual(float(rows[0][1]), 56.44, places=1)
        self.assertIsInstance(rows[0][2], int)
        self.assertEqual(len(rows[0][3]), 2)

        # ANY needs COUNT and may return any single matching member.
        any_one = self.cmd("GEOSEARCH", g, "FROMLONLAT", "15", "37",
                           "BYRADIUS", "200", "km", "COUNT", "1", "ANY")
        self.assertEqual(len(any_one), 1)
        self.assertIn(any_one[0], (b"catania", b"palermo"))
        with self.assertRaises(RedisError):
            self.cmd("GEOSEARCH", g, "FROMLONLAT", "15", "37",
                     "BYRADIUS", "200", "km", "ANY")
        with self.assertRaises(RedisError):
            self.cmd("GEOSEARCH", g, "FROMMEMBER", "palermo",
                     "FROMLONLAT", "15", "37", "BYRADIUS", "200", "km")
        with self.assertRaises(RedisError):
            self.cmd("GEOSEARCH", g, "FROMMEMBER", "palermo",
                     "BYRADIUS", "200", "km", "BYBOX", "1", "1", "km")

        dst = self.key("geo:search:dst")
        self.assertEqual(self.cmd("GEOSEARCHSTORE", dst, g,
                                  "FROMLONLAT", "15", "37",
                                  "BYRADIUS", "200", "km", "STOREDIST"), 2)
        self.assertEqual(self.cmd("ZRANGE", dst, "0", "-1", "WITHSCORES"),
                         [b"catania", b"56.441257870158204",
                          b"palermo", b"190.44242984775784"])

        # A missing source deletes the destination and returns zero.
        missing = self.key("geo:search:missing")
        self.assertEqual(self.cmd("GEOSEARCHSTORE", dst, missing,
                                  "FROMMEMBER", "x",
                                  "BYRADIUS", "1", "km"), 0)
        self.assertEqual(self.cmd("EXISTS", dst), 0)

        # The old GEORADIUS command keeps working on top of the new engine.
        self.assertEqual(self.cmd("GEORADIUS", g, "15", "37", "200", "km",
                                  "ASC"), [b"catania", b"palermo"])

    def test_acl_subcommands(self):
        categories = self.cmd("ACL", "CAT")
        for name in (b"keyspace", b"read", b"write", b"admin", b"pubsub",
                     b"fast", b"slow"):
            self.assertIn(name, categories)
        reads = self.cmd("ACL", "CAT", "read")
        self.assertIn(b"get", reads)
        self.assertNotIn(b"set", reads)
        with self.assertRaises(RedisError):
            self.cmd("ACL", "CAT", "nosuchcategory")

        name = "bp8acluser"
        self.assertEqual(self.cmd("ACL", "SETUSER", name, "on", ">pw",
                                  "~pref:*", "+get", "+set"), b"OK")
        self.assertIn(name.encode(), self.cmd("ACL", "USERS"))

        info = self.flat_map(self.cmd("ACL", "GETUSER", name))
        self.assertEqual(info[b"flags"], [b"on"])
        self.assertEqual(info[b"keys"], [b"~pref:*"])
        self.assertEqual(len(info[b"passwords"]), 1)
        self.assertIn(b"-@all", info[b"commands"])
        self.assertIn(b"+get", info[b"commands"])
        self.assertEqual(info[b"channels"], [])

        self.assertEqual(len(self.cmd("ACL", "GENPASS")), 64)
        self.assertEqual(len(self.cmd("ACL", "GENPASS", "8")), 2)

        self.assertEqual(self.cmd("ACL", "DRYRUN", name, "get", "pref:1"),
                         b"OK")
        with self.assertRaises(RedisError):
            self.cmd("ACL", "DRYRUN", name, "del", "pref:1")
        with self.assertRaises(RedisError):
            self.cmd("ACL", "DRYRUN", name, "get", "other:1")

        self.assertEqual(self.cmd("ACL", "DELUSER", name), 1)
        self.assertNotIn(name.encode(), self.cmd("ACL", "USERS"))

        self.assertEqual(self.cmd("ACL", "WHOAMI"), b"default")
        help_text = self.cmd("ACL", "HELP")
        self.assertTrue(any(b"WHOAMI" in line for line in help_text))
        with self.assertRaises(RedisError):
            self.cmd("ACL", "WHOAMI", "extra")

    def test_client_kill_options_and_list_fields(self):
        if self.server is None:
            self.skipTest("CLIENT tests require --server")
        info = self.cmd("CLIENT", "INFO")
        self.assertIn(("laddr=127.0.0.1:%d" % self.port).encode(), info)
        self.assertIn(b"user=default", info)

        name = "bp8killuser"
        self.assertEqual(self.cmd("ACL", "SETUSER", name, "on", ">pw",
                                  "+@all", "~*"), b"OK")
        self.addCleanup(self.cmd, "ACL", "DELUSER", name)

        with self.connect() as user_client:
            self.assertEqual(user_client.command("AUTH", name, "pw"), b"OK")
            self.assertIn(b"user=" + name.encode(), self.cmd("CLIENT", "LIST"))
            self.assertEqual(self.cmd("CLIENT", "KILL", "USER", name), 1)
            with self.assertRaises((OSError, EOFError)):
                user_client.command("PING")
        self.assertEqual(self.cmd("CLIENT", "KILL", "USER", "nosuchuser"), 0)

        with self.connect() as victim:
            self.assertEqual(victim.command("PING"), b"PONG")
            self.assertEqual(self.cmd("CLIENT", "KILL", "MAXAGE", "99999"), 0)
            # MAXAGE 0 matches every other connection; count can include a
            # peer that is already closing after KILL USER on Windows.
            killed = self.cmd("CLIENT", "KILL", "MAXAGE", "0")
            self.assertGreaterEqual(killed, 1)
            with self.assertRaises((OSError, EOFError)):
                victim.command("PING")
        with self.assertRaises(RedisError):
            self.cmd("CLIENT", "KILL", "MAXAGE", "-1")

        # laddr is the address the server accepted on, so it matches every
        # connection; skipme keeps the issuing client alive.
        with self.connect() as victim:
            victim_info = victim.command("CLIENT", "INFO")
            laddr = [field for field in victim_info.split()
                     if field.startswith(b"laddr=")][0][len(b"laddr="):]
            self.assertEqual(self.cmd("CLIENT", "KILL", "LADDR", laddr), 1)
            with self.assertRaises((OSError, EOFError)):
                victim.command("PING")

    def test_info_everything_and_wait_aof_local(self):
        if self.server is None:
            self.skipTest("INFO/WAITAOF tests require --server")
        info = self.cmd("INFO", "everything")
        for section in (b"# Server", b"# Clients", b"# Commandstats",
                        b"# Keyspace"):
            self.assertIn(section, info)

        # appendfsync is "always" for the managed test server, so the local
        # AOF is acknowledged synchronously.
        self.assertEqual(self.cmd("WAITAOF", "1", "0", "100"), [1, 0])
        self.assertEqual(self.cmd("WAITAOF", "0", "0", "100"), [0, 0])
        self.assertEqual(self.cmd("WAITAOF", "1", "2", "100"), [1, 0])
        with self.assertRaises(RedisError):
            self.cmd("WAITAOF", "-1", "0", "100")
        with self.assertRaises(RedisError):
            self.cmd("WAITAOF", "0", "-1", "100")

    def test_wait_durability_without_appendonly(self):
        if self.server is None:
            self.skipTest("WAITAOF test requires --server")
        self.assertEqual(self.cmd("WAITAOF", "0", "0", "100"), [0, 0])
        with self.assertRaises(RedisError):
            self.cmd("WAITAOF", "1", "0", "100")

    def test_shutdown_option_validation(self):
        if self.server is None:
            self.skipTest("SHUTDOWN tests require --server")
        with self.assertRaises(RedisError) as raised:
            self.cmd("SHUTDOWN", "ABORT")
        self.assertIn("No shutdown in progress", str(raised.exception))
        for args in (("SHUTDOWN", "ABORT", "SAVE"),
                     ("SHUTDOWN", "ABORT", "NOSAVE"),
                     ("SHUTDOWN", "SAVE", "NOSAVE"),
                     ("SHUTDOWN", "NOSAVE", "BOGUS")):
            with self.assertRaises(RedisError):
                self.cmd(*args)

        # NOW and FORCE are accepted and the extra server exits.
        directory = Path(self.temp.name) / "shutdown"
        directory.mkdir()
        extra = ManagedServer(self.server.executable, directory,
                              OPTIONS.timeout, OPTIONS.startup_timeout)
        self.addCleanup(extra.close)
        extra.start()
        client = RESP2("127.0.0.1", extra.port, OPTIONS.timeout)
        try:
            client.command("SHUTDOWN", "NOW", "FORCE")
        except (OSError, EOFError):
            pass
        finally:
            client.close()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and extra.process.poll() is None:
            time.sleep(0.05)
        self.assertIsNotNone(extra.process.poll())

    def test_acl_log_and_acl_list(self):
        self.assertEqual(self.cmd("ACL", "SETUSER", "bp9limited", "on", ">pw",
                                  "~ok:*", "+get"), b"OK")
        line = [entry for entry in self.cmd("ACL", "LIST")
                if entry.startswith(b"user bp9limited")][0]
        self.assertIn(b"~ok:*", line)
        self.assertIn(b"-@all", line)
        self.assertIn(b"+get", line)

        limited = self.connect()
        self.addCleanup(limited.close)
        self.assertEqual(limited.command("AUTH", "bp9limited", "pw"), b"OK")
        with self.assertRaises(RedisError):
            limited.command("SET", "ok:1", "1")
        with self.assertRaises(RedisError):
            limited.command("GET", "bad:1")

        log = self.cmd("ACL", "LOG")
        reasons = {self.flat_map(entry)[b"reason"] for entry in log}
        self.assertEqual(reasons, {b"command", b"key"})
        command_entry = [self.flat_map(entry) for entry in log
                         if self.flat_map(entry)[b"reason"] == b"command"][0]
        self.assertEqual(command_entry[b"object"], b"set")
        self.assertEqual(command_entry[b"username"], b"bp9limited")
        self.assertIn(b"client-info", command_entry)
        self.assertEqual(command_entry[b"context"], b"toplevel")

        # Identical denials are grouped into one entry.
        for _ in range(2):
            with self.assertRaises(RedisError):
                limited.command("SET", "ok:1", "1")
        log = self.cmd("ACL", "LOG")
        command_entry = [self.flat_map(entry) for entry in log
                         if self.flat_map(entry)[b"reason"] == b"command"][0]
        self.assertEqual(command_entry[b"count"], 3)

        failed = self.connect()
        with self.assertRaises(RedisError):
            failed.command("AUTH", "bp9limited", "wrong")
        failed.close()
        log = self.cmd("ACL", "LOG")
        auth_entry = [self.flat_map(entry) for entry in log
                      if self.flat_map(entry)[b"reason"] == b"auth"][0]
        self.assertEqual(auth_entry[b"object"], b"bp9limited")

        self.assertEqual(len(self.cmd("ACL", "LOG", "1")), 1)
        self.assertEqual(self.cmd("ACL", "LOG", "RESET"), b"OK")
        self.assertEqual(self.cmd("ACL", "LOG"), [])

        # acllog-max-len 0 disables logging.
        self.cmd("CONFIG", "SET", "acllog-max-len", "0")
        with self.assertRaises(RedisError):
            limited.command("SET", "ok:1", "1")
        self.assertEqual(self.cmd("ACL", "LOG"), [])
        self.cmd("CONFIG", "SET", "acllog-max-len", "128")
        self.assertEqual(self.cmd("ACL", "DELUSER", "bp9limited"), 1)

    def test_acl_save_load_without_file(self):
        for subcommand in ("SAVE", "LOAD"):
            with self.assertRaises(RedisError):
                self.cmd("ACL", subcommand)

    def test_aclfile_save_and_load(self):
        if self.server is None:
            self.skipTest("ACL file test requires --server")
        self.assertIn(b"default", self.cmd("ACL", "USERS"))
        self.assertEqual(self.cmd("ACL", "SETUSER", "bp9file", "on", ">pw",
                                  "~x:*", "+get", "resetchannels",
                                  "&x:*"), b"OK")
        self.assertEqual(self.cmd("ACL", "SAVE"), b"OK")

        text = (self.server.directory / "acl.conf").read_text()
        self.assertIn("user bp9file on", text)
        self.assertIn("~x:*", text)
        self.assertIn("resetchannels &x:*", text)
        self.assertIn("-@all +get", text)

        self.assertEqual(self.cmd("ACL", "DELUSER", "bp9file"), 1)
        self.assertNotIn(b"bp9file", self.cmd("ACL", "USERS"))
        self.assertEqual(self.cmd("ACL", "LOAD"), b"OK")
        self.assertIn(b"bp9file", self.cmd("ACL", "USERS"))
        info = self.flat_map(self.cmd("ACL", "GETUSER", "bp9file"))
        self.assertEqual(info[b"keys"], [b"~x:*"])
        self.assertEqual(info[b"channels"], [b"&x:*"])
        self.assertIn(b"+get", info[b"commands"])

        probe = self.connect()
        self.addCleanup(probe.close)
        self.assertEqual(probe.command("AUTH", "bp9file", "pw"), b"OK")
        self.assertEqual(probe.command("GET", "x:1"), None)

    def test_client_setinfo_and_list(self):
        if self.server is None:
            self.skipTest("CLIENT test requires --server")
        info = self.cmd("CLIENT", "INFO")
        self.assertIn(b"lib-name=", info)
        self.assertIn(b"lib-ver=", info)

        self.assertEqual(self.cmd("CLIENT", "SETINFO", "lib-name", "myapp"),
                         b"OK")
        self.assertEqual(self.cmd("CLIENT", "SETINFO", "lib-ver", "9.9.9"),
                         b"OK")
        info = self.cmd("CLIENT", "INFO")
        self.assertIn(b"lib-name=myapp", info)
        self.assertIn(b"lib-ver=9.9.9", info)
        listing = self.cmd("CLIENT", "LIST")
        self.assertIn(b"lib-name=myapp lib-ver=9.9.9", listing)

        with self.assertRaises(RedisError):
            self.cmd("CLIENT", "SETINFO", "lib-name", "x" * 129)
        with self.assertRaises(RedisError):
            self.cmd("CLIENT", "SETINFO", "lib-name", "has space")
        with self.assertRaises(RedisError):
            self.cmd("CLIENT", "SETINFO", "bogus", "x")

        self.assertEqual(self.cmd("RESET"), b"OK")
        info = self.cmd("CLIENT", "INFO")
        self.assertIn(b"lib-name= ", info)
        self.assertIn(b"lib-ver=", info)

    def test_xgroup_setid_entriesread(self):
        key = self.key("stream:entriesread")
        for ms in (1, 2, 3):
            self.cmd("XADD", key, "%d-1" % ms, "f", ms)
        self.assertEqual(self.cmd("XGROUP", "CREATE", key, "g", "0"), b"OK")
        groups = self.flat_map(self.cmd("XINFO", "GROUPS", key)[0])
        self.assertEqual(groups[b"entries-read"], 0)
        self.assertEqual(groups[b"lag"], 3)

        self.assertEqual(self.cmd("XGROUP", "SETID", key, "g", "2-1",
                                  "ENTRIESREAD", "2"), b"OK")
        groups = self.flat_map(self.cmd("XINFO", "GROUPS", key)[0])
        self.assertEqual(groups[b"last-delivered-id"], b"2-1")
        self.assertEqual(groups[b"entries-read"], 2)
        self.assertEqual(groups[b"lag"], 1)

        reply = self.cmd("XREADGROUP", "GROUP", "g", "c", "COUNT", "1",
                         "STREAMS", key, ">")
        self.assertEqual(reply[0][1][0][0], b"3-1")
        groups = self.flat_map(self.cmd("XINFO", "GROUPS", key)[0])
        self.assertEqual(groups[b"entries-read"], 3)
        self.assertEqual(groups[b"lag"], 0)

        with self.assertRaises(RedisError):
            self.cmd("XGROUP", "SETID", key, "g", "2-1", "ENTRIESREAD", "-1")
        with self.assertRaises(RedisError):
            self.cmd("XGROUP", "SETID", key, "g", "2-1", "BOGUS", "1")

    def test_info_errorstats(self):
        key = self.key("errorstats")
        self.cmd("SET", key, "value")
        with self.assertRaises(RedisError):
            self.cmd("LPUSH", key, "x")
        with self.assertRaises(RedisError):
            self.cmd("GET")
        info = self.cmd("INFO", "errorstats")
        self.assertIn(b"# Errorstats", info)
        self.assertIn(b"errorstat_WRONGTYPE:count=", info)
        self.assertIn(b"errorstat_ERR:count=", info)

        self.assertEqual(self.cmd("CONFIG", "RESETSTAT"), b"OK")
        info = self.cmd("INFO", "errorstats")
        self.assertNotIn(b"errorstat_WRONGTYPE", info)

    def test_sharded_pubsub(self):
        if self.server is None:
            self.skipTest("Pub/Sub test requires --server")
        sub = self.connect()
        self.addCleanup(sub.close)
        reply = sub.command("SSUBSCRIBE", "shard:1")
        self.assertEqual(reply[0], b"ssubscribe")
        self.assertEqual(reply[1], b"shard:1")
        self.assertEqual(reply[2], 1)
        self.assertEqual(self.cmd("PUBSUB", "SHARDCHANNELS"), [b"shard:1"])
        self.assertEqual(self.cmd("PUBSUB", "SHARDNUMSUB", "shard:1", "none"),
                         [b"shard:1", 1, b"none", 0])
        self.assertEqual(self.cmd("SPUBLISH", "shard:1", "hello"), 1)
        self.assertEqual(sub._read(), [b"smessage", b"shard:1", b"hello"])
        # Shard channels are separate from regular channels.
        self.assertNotIn(b"shard:1", self.cmd("PUBSUB", "CHANNELS"))

        reply = sub.command("SUNSUBSCRIBE", "shard:1")
        self.assertEqual(reply[0], b"sunsubscribe")
        self.assertEqual(reply[1], b"shard:1")
        self.assertEqual(self.cmd("PUBSUB", "SHARDCHANNELS"), [])

        # RESET is allowed from the Pub/Sub context and drops subscriptions.
        sub.command("SSUBSCRIBE", "shard:2")
        self.assertEqual(sub.command("RESET"), b"OK")
        self.assertEqual(self.cmd("PUBSUB", "SHARDCHANNELS"), [])

    def test_acl_deluser_disconnects_clients(self):
        if self.server is None:
            self.skipTest("CLIENT test requires --server")
        self.assertEqual(self.cmd("ACL", "SETUSER", "bp9victim", "on", ">pw",
                                  "+@all", "~*"), b"OK")
        victim = self.connect()
        self.addCleanup(victim.close)
        self.assertEqual(victim.command("AUTH", "bp9victim", "pw"), b"OK")
        self.assertEqual(self.cmd("ACL", "DELUSER", "bp9victim"), 1)
        with self.assertRaises((OSError, EOFError)):
            victim.command("PING")
        self.assertEqual(self.cmd("PING"), b"PONG")

    def test_latency_histogram_and_latencystats(self):
        if self.server is None:
            self.skipTest("latency tracking test requires --server")
        self.assertEqual(self.cmd("CONFIG", "GET", "latency-tracking"),
                         [b"latency-tracking", b"yes"])
        for _ in range(3):
            self.cmd("GET", "missing:bp10")

        histogram = self.flat_map(self.cmd("LATENCY", "HISTOGRAM", "get"))
        self.assertIn(b"get", histogram)
        entry = self.flat_map(histogram[b"get"])
        self.assertGreaterEqual(entry[b"calls"], 3)
        buckets = entry[b"histogram_usec"]
        self.assertTrue(buckets)
        self.assertEqual(len(buckets) % 2, 0)
        micros, counts = buckets[0::2], buckets[1::2]
        self.assertEqual(micros, sorted(micros))
        self.assertEqual(counts, sorted(counts))
        self.assertEqual(counts[-1], entry[b"calls"])

        info = self.cmd("INFO", "latencystats")
        self.assertIn(b"latency_percentiles_usec_get:p50=", info)

        # Both reset paths clear the histograms.
        self.cmd("CONFIG", "RESETSTAT")
        self.assertEqual(self.cmd("LATENCY", "HISTOGRAM", "get"), [])
        self.cmd("GET", "missing:bp10")
        self.assertTrue(self.cmd("LATENCY", "HISTOGRAM", "get"))
        self.cmd("LATENCY", "RESET")
        self.assertEqual(self.cmd("LATENCY", "HISTOGRAM", "get"), [])

        # Turning tracking off stops new samples without clearing old ones.
        self.cmd("GET", "missing:bp10")
        before = self.flat_map(self.flat_map(
            self.cmd("LATENCY", "HISTOGRAM", "get"))[b"get"])[b"calls"]
        self.assertEqual(self.cmd("CONFIG", "SET", "latency-tracking", "no"),
                         b"OK")
        for _ in range(3):
            self.cmd("GET", "missing:bp10")
        after = self.flat_map(self.flat_map(
            self.cmd("LATENCY", "HISTOGRAM", "get"))[b"get"])[b"calls"]
        self.assertEqual(before, after)
        self.assertEqual(self.cmd("CONFIG", "SET", "latency-tracking", "yes"),
                         b"OK")

    def test_acl_channel_permissions(self):
        self.assertEqual(self.cmd("ACL", "SETUSER", "bp10pub", "on", ">pw",
                                  "~*", "+@all", "resetchannels",
                                  "&news:*"), b"OK")
        line = [entry for entry in self.cmd("ACL", "LIST")
                if entry.startswith(b"user bp10pub")][0]
        self.assertIn(b"resetchannels", line)
        self.assertIn(b"&news:*", line)
        info = self.flat_map(self.cmd("ACL", "GETUSER", "bp10pub"))
        self.assertEqual(info[b"channels"], [b"&news:*"])

        pub = self.connect()
        self.addCleanup(pub.close)
        self.assertEqual(pub.command("AUTH", "bp10pub", "pw"), b"OK")
        self.assertEqual(pub.command("PUBLISH", "news:1", "x"), 0)
        with self.assertRaises(RedisError):
            pub.command("PUBLISH", "other", "x")
        with self.assertRaises(RedisError):
            pub.command("SPUBLISH", "other", "x")
        self.assertEqual(pub.command("SUBSCRIBE", "news:1")[0],
                         b"subscribe")
        with self.assertRaises(RedisError):
            pub.command("SUBSCRIBE", "other")
        self.assertEqual(pub.command("PSUBSCRIBE", "news:*")[0],
                         b"psubscribe")
        with self.assertRaises(RedisError):
            pub.command("PSUBSCRIBE", "news:1")
        self.assertEqual(pub.command("SSUBSCRIBE", "news:2")[0],
                         b"ssubscribe")
        # All channels are checked before anything is subscribed.
        with self.assertRaises(RedisError):
            pub.command("SUBSCRIBE", "news:3", "other")

        log = self.cmd("ACL", "LOG")
        channel_entry = [self.flat_map(entry) for entry in log
                         if self.flat_map(entry)[b"reason"] == b"channel"][0]
        self.assertEqual(channel_entry[b"object"], b"other")
        self.assertEqual(channel_entry[b"username"], b"bp10pub")
        default = self.flat_map(self.cmd("ACL", "GETUSER", "default"))
        self.assertEqual(default[b"channels"], [b"&*"])

    def test_script_flush_options(self):
        sha = self.cmd("SCRIPT", "LOAD", "return 1")
        self.assertEqual(self.cmd("SCRIPT", "EXISTS", sha), [1])
        self.assertEqual(self.cmd("SCRIPT", "FLUSH", "ASYNC"), b"OK")
        self.assertEqual(self.cmd("SCRIPT", "EXISTS", sha), [0])
        sha = self.cmd("SCRIPT", "LOAD", "return 2")
        self.assertEqual(self.cmd("SCRIPT", "FLUSH", "SYNC"), b"OK")
        self.assertEqual(self.cmd("SCRIPT", "EXISTS", sha), [0])
        with self.assertRaises(RedisError):
            self.cmd("SCRIPT", "FLUSH", "BOGUS")

    def test_acl_applies_inside_scripts(self):
        self.assertEqual(self.cmd("ACL", "SETUSER", "bp10lua", "on", ">pw",
                                  "~ok:*", "+eval", "+get"), b"OK")
        lua = self.connect()
        self.addCleanup(lua.close)
        self.assertEqual(lua.command("AUTH", "bp10lua", "pw"), b"OK")
        self.assertIsNone(lua.command(
            "EVAL", "return redis.call('get','ok:1')", 0))
        with self.assertRaises(RedisError):
            lua.command("EVAL", "return redis.call('set','ok:1','x')", 0)
        with self.assertRaises(RedisError):
            lua.command("EVAL", "return redis.call('get','bad:1')", 0)
        # The same rule applies to channels used from a script.
        self.assertEqual(self.cmd("ACL", "SETUSER", "bp10lua", "+publish"),
                         b"OK")
        with self.assertRaises(RedisError):
            lua.command("EVAL", "return redis.call('publish','any','x')", 0)
        log = self.cmd("ACL", "LOG")
        contexts = {self.flat_map(entry)[b"context"] for entry in log}
        self.assertIn(b"lua", contexts)

    def test_requirepass_rejects_wrong_password(self):
        if self.server is None:
            self.skipTest("requirepass test requires --server")
        directory = Path(self.temp.name) / "requirepass"
        directory.mkdir()
        server = ManagedServer(OPTIONS.server, directory, OPTIONS.timeout,
                               OPTIONS.startup_timeout, requirepass="admin")
        self.addCleanup(server.close)
        server.start()
        with self.assertRaises(RedisError):
            RESP2("127.0.0.1", server.port, OPTIONS.timeout, "wrong")
        admin = RESP2("127.0.0.1", server.port, OPTIONS.timeout, "admin")
        self.addCleanup(admin.close)
        self.assertEqual(admin.command("PING"), b"PONG")

        # CONFIG SET requirepass updates the default ACL user as well.
        self.assertEqual(admin.command("CONFIG", "SET", "requirepass",
                                       "newpass"), b"OK")
        with self.assertRaises(RedisError):
            RESP2("127.0.0.1", server.port, OPTIONS.timeout, "admin")
        rotated = RESP2("127.0.0.1", server.port, OPTIONS.timeout, "newpass")
        self.addCleanup(rotated.close)
        self.assertEqual(rotated.command("PING"), b"PONG")

    def test_scan_type_filter(self):
        skey = self.key("scan:type:str")
        hkey = self.key("scan:type:hash")
        lkey = self.key("scan:type:list")
        pattern = self.prefix + b"scan:type:*"
        self.cmd("DEL", skey, hkey, lkey)
        self.cmd("SET", skey, "v")
        self.cmd("HSET", hkey, "f", "1")
        self.cmd("LPUSH", lkey, "x")

        def scan_all(typ=None):
            cursor = b"0"
            found = set()
            for _ in range(1000):
                args = ["SCAN", cursor, "MATCH", pattern, "COUNT", "100"]
                if typ is not None:
                    args.extend(["TYPE", typ])
                cursor, keys = self.cmd(*args)
                found.update(keys)
                if cursor in (b"0", 0):
                    break
            return found

        self.assertEqual(scan_all("string"), {skey})
        self.assertEqual(scan_all("hash"), {hkey})
        self.assertEqual(scan_all("list"), {lkey})
        self.assertEqual(scan_all("nosuchtype"), set())
        with self.assertRaises(RedisError):
            self.cmd("HSCAN", hkey, "0", "TYPE", "string")

    def test_hello_resp2(self):
        reply = self.cmd("HELLO")
        info = self.flat_map(reply)
        self.assertEqual(info[b"server"], b"redis")
        self.assertIn(b"bp1", info[b"version"])
        self.assertEqual(info[b"proto"], 2)
        self.assertEqual(info[b"mode"], b"standalone")
        self.assertEqual(info[b"role"], b"master")
        self.assertEqual(info[b"modules"], [])
        self.assertIn(b"id", info)

        reply2 = self.cmd("HELLO", "2", "SETNAME", "bp12-client")
        info2 = self.flat_map(reply2)
        self.assertEqual(info2[b"proto"], 2)
        self.assertEqual(self.cmd("CLIENT", "GETNAME"), b"bp12-client")

        with self.assertRaises(RedisError) as ctx:
            self.cmd("HELLO", "3")
        self.assertIn("NOPROTO", str(ctx.exception))

        if self.server is None:
            return
        directory = Path(self.temp.name) / "hello-auth"
        directory.mkdir()
        guarded = ManagedServer(OPTIONS.server, directory, OPTIONS.timeout,
                                OPTIONS.startup_timeout, requirepass="secret")
        self.addCleanup(guarded.close)
        guarded.start()
        raw = RESP2("127.0.0.1", guarded.port, OPTIONS.timeout)
        self.addCleanup(raw.close)
        with self.assertRaises(RedisError):
            raw.command("HELLO", "2")
        hello = raw.command("HELLO", "2", "AUTH", "default", "secret",
                            "SETNAME", "authed")
        mapped = self.flat_map(hello)
        self.assertEqual(mapped[b"proto"], 2)
        self.assertEqual(raw.command("PING"), b"PONG")
        self.assertEqual(raw.command("CLIENT", "GETNAME"), b"authed")
        with self.assertRaises(RedisError):
            raw.command("HELLO", "2", "AUTH", "default", "wrong")

    def test_xinfo_consumers_inactive(self):
        key = self.key("xinfo:inactive")
        self.cmd("DEL", key)
        self.cmd("XADD", key, "1-0", "f", "1")
        self.cmd("XGROUP", "CREATE", key, "g", "0")

        # Creating a consumer without delivering updates idle; inactive stays -1.
        self.cmd("XGROUP", "CREATECONSUMER", key, "g", "c1")
        rows = self.cmd("XINFO", "CONSUMERS", key, "g")
        self.assertEqual(len(rows), 1)
        consumer = self.flat_map(rows[0])
        self.assertEqual(consumer[b"name"], b"c1")
        self.assertEqual(consumer[b"pending"], 0)
        self.assertGreaterEqual(consumer[b"idle"], 0)
        self.assertEqual(consumer[b"inactive"], -1)

        self.cmd("XREADGROUP", "GROUP", "g", "c1", "COUNT", "1",
                 "STREAMS", key, ">")
        rows = self.cmd("XINFO", "CONSUMERS", key, "g")
        consumer = self.flat_map(rows[0])
        self.assertEqual(consumer[b"pending"], 1)
        self.assertGreaterEqual(consumer[b"inactive"], 0)

        # An empty read refreshes idle (seen_time) but not inactive.
        time.sleep(0.05)
        self.cmd("XREADGROUP", "GROUP", "g", "c1", "COUNT", "1",
                 "STREAMS", key, ">")
        rows = self.cmd("XINFO", "CONSUMERS", key, "g")
        consumer = self.flat_map(rows[0])
        # Empty XREADGROUP refreshes idle only; allow 2ms Windows timer jitter.
        self.assertGreaterEqual(consumer[b"inactive"] + 2, consumer[b"idle"])

        full = self.flat_map(self.cmd("XINFO", "STREAM", key, "FULL", "COUNT", "1"))
        group = self.flat_map(full[b"groups"][0])
        cfull = self.flat_map(group[b"consumers"][0])
        self.assertIn(b"active-time", cfull)
        self.assertGreater(cfull[b"active-time"], 0)

    def test_client_tracking_pubsub_invalidate(self):
        if self.server is None:
            self.skipTest("CLIENT TRACKING test requires --server")

        key = self.key("track:k")
        self.cmd("DEL", key)

        # Invalidation peer must be in Pub/Sub; TRACKING REDIRECT points here.
        # CLIENT ID must be read before SUBSCRIBE (Pub/Sub context rejects it).
        sub = self.connect()
        self.addCleanup(sub.close)
        redir_id = sub.command("CLIENT", "ID")
        self.assertEqual(sub.command("SUBSCRIBE", "__redis__:invalidate")[0],
                         b"subscribe")

        data = self.connect()
        self.addCleanup(data.close)
        self.assertEqual(
            data.command("CLIENT", "TRACKING", "ON", "REDIRECT", str(redir_id)),
            b"OK")
        self.assertEqual(data.command("CLIENT", "GETREDIR"), redir_id)

        info = data.command("CLIENT", "TRACKINGINFO")
        self.assertEqual(info[0], b"flags")
        self.assertIn(b"on", info[1])
        self.assertEqual(info[2], b"redirect")
        self.assertEqual(info[3], redir_id)

        self.assertEqual(data.command("GET", key), None)
        self.assertEqual(self.cmd("SET", key, "v1"), b"OK")

        # Invalidation is queued on the redirect client and flushed in beforeSleep.
        time.sleep(0.05)
        msg = sub._read()
        self.assertEqual(msg[0], b"message")
        self.assertEqual(msg[1], b"__redis__:invalidate")
        self.assertEqual(msg[2], [key])

        # OPTIN: keys are remembered only after CACHING YES.
        data2 = self.connect()
        self.addCleanup(data2.close)
        self.assertEqual(
            data2.command("CLIENT", "TRACKING", "ON", "OPTIN",
                          "REDIRECT", str(redir_id)),
            b"OK")
        key2 = self.key("track:optin")
        self.cmd("DEL", key2)
        self.assertEqual(data2.command("GET", key2), None)
        self.assertEqual(self.cmd("SET", key2, "x"), b"OK")
        # No CACHING YES -> no invalidation. Probe with select so a timeout
        # does not poison the buffered reader used by later asserts.
        time.sleep(0.05)
        readable, _, _ = select.select([sub.sock], [], [], 0.2)
        self.assertEqual(readable, [])

        self.assertEqual(data2.command("CLIENT", "CACHING", "YES"), b"OK")
        self.assertEqual(data2.command("GET", key2), b"x")
        self.assertEqual(self.cmd("SET", key2, "y"), b"OK")
        time.sleep(0.05)
        msg = sub._read()
        self.assertEqual(msg[0], b"message")
        self.assertEqual(msg[2], [key2])

        self.assertEqual(data.command("CLIENT", "TRACKING", "OFF"), b"OK")
        self.assertEqual(data.command("CLIENT", "GETREDIR"), -1)

    def test_client_tracking_bcast_config_and_stats(self):
        if self.server is None:
            self.skipTest("CLIENT TRACKING BCAST test requires --server")

        # CONFIG GET/SET for tracking-table-max-keys
        self.assertEqual(
            self.cmd("CONFIG", "GET", "tracking-table-max-keys"),
            [b"tracking-table-max-keys", b"1000000"])
        self.assertEqual(
            self.cmd("CONFIG", "SET", "tracking-table-max-keys", "42"), b"OK")
        self.assertEqual(
            self.cmd("CONFIG", "GET", "tracking-table-max-keys"),
            [b"tracking-table-max-keys", b"42"])
        self.assertEqual(
            self.cmd("CONFIG", "SET", "tracking-table-max-keys", "1000000"),
            b"OK")

        # BCAST + PREFIX invalidation via Pub/Sub redirect.
        prefix = self.key("bcast:")
        key = prefix + b"one"
        self.cmd("DEL", key)

        sub = self.connect()
        self.addCleanup(sub.close)
        redir_id = sub.command("CLIENT", "ID")
        self.assertEqual(sub.command("SUBSCRIBE", "__redis__:invalidate")[0],
                         b"subscribe")

        data = self.connect()
        self.addCleanup(data.close)
        self.assertEqual(
            data.command("CLIENT", "TRACKING", "ON", "BCAST",
                         "PREFIX", prefix,
                         "REDIRECT", str(redir_id)),
            b"OK")

        info = data.command("CLIENT", "TRACKINGINFO")
        self.assertEqual(info[0], b"flags")
        self.assertIn(b"on", info[1])
        self.assertIn(b"bcast", info[1])

        clients = self.cmd("INFO", "clients")
        self.assertIn(b"tracking_clients:", clients)
        self.assertRegex(clients, br"tracking_clients:[1-9]")

        self.assertEqual(self.cmd("SET", key, "v1"), b"OK")
        time.sleep(0.05)
        msg = sub._read()
        self.assertEqual(msg[0], b"message")
        self.assertEqual(msg[1], b"__redis__:invalidate")
        self.assertEqual(msg[2], [key])

        stats = self.cmd("INFO", "stats")
        self.assertIn(b"tracking_total_keys:", stats)
        self.assertIn(b"tracking_total_items:", stats)
        self.assertIn(b"tracking_total_prefixes:", stats)
        self.assertRegex(stats, br"tracking_total_prefixes:[1-9]")

        mem = self.flat_map(self.cmd("MEMORY", "STATS"))
        self.assertIn(b"clients.tracking.table.keys", mem)
        self.assertIn(b"clients.tracking.table.items", mem)
        self.assertIn(b"clients.tracking.table.prefixes", mem)
        self.assertGreaterEqual(mem[b"clients.tracking.table.prefixes"], 1)

        clist = self.cmd("CLIENT", "LIST")
        self.assertIn(b"flags=", clist)
        self.assertRegex(clist, br"flags=[^\r\n]*t")

        self.assertEqual(data.command("CLIENT", "TRACKING", "OFF"), b"OK")

    def test_quit_script_help_and_command_info(self):
        # QUIT is a registered command (COMMAND INFO / COUNT see it).
        info = self.cmd("COMMAND", "INFO", "quit", "ping")
        self.assertEqual(len(info), 2)
        self.assertEqual(info[0][0], b"quit")
        self.assertEqual(info[1][0], b"ping")

        help_txt = self.cmd("SCRIPT", "HELP")
        joined = b" ".join(help_txt).lower()
        self.assertIn(b"flush", joined)
        self.assertIn(b"load", joined)
        self.assertIn(b"exists", joined)

        # QUIT closes after OK; a fresh connection still works.
        sock = self.connect()
        self.assertEqual(sock.command("QUIT"), b"OK")
        sock.close()
        self.assertEqual(self.cmd("PING"), b"PONG")

    def test_ops_observability_helpers(self):
        if self.server is None:
            self.skipTest("ops observability tests require --server")

        # Seed a latency sample: threshold is ms; DEBUG SLEEP uses seconds.
        self.assertEqual(
            self.cmd("CONFIG", "SET", "latency-monitor-threshold", "50"), b"OK")
        self.cmd("DEBUG", "SLEEP", "0.15")
        history = self.cmd("LATENCY", "HISTORY", "command")
        self.assertIsInstance(history, list)
        self.assertTrue(history)
        latest = self.cmd("LATENCY", "LATEST")
        self.assertIsInstance(latest, list)
        self.assertTrue(any(row and row[0] == b"command" for row in latest))
        graph = self.cmd("LATENCY", "GRAPH", "command")
        self.assertIsInstance(graph, (bytes, bytearray))
        self.assertIn(b"command", graph)
        doctor = self.cmd("LATENCY", "DOCTOR")
        self.assertIsInstance(doctor, (bytes, bytearray))
        help_txt = b" ".join(self.cmd("LATENCY", "HELP")).lower()
        self.assertIn(b"doctor", help_txt)
        self.assertIn(b"graph", help_txt)
        self.assertEqual(
            self.cmd("CONFIG", "SET", "latency-monitor-threshold", "0"), b"OK")

        mem_help = b" ".join(self.cmd("MEMORY", "HELP")).lower()
        self.assertIn(b"stats", mem_help)
        self.assertIn(b"doctor", mem_help)
        doctor_mem = self.cmd("MEMORY", "DOCTOR")
        self.assertIsInstance(doctor_mem, (bytes, bytearray))
        self.assertTrue(len(doctor_mem) > 0)
        malloc_stats = self.cmd("MEMORY", "MALLOC-STATS")
        self.assertIsInstance(malloc_stats, (bytes, bytearray))
        self.assertEqual(self.cmd("MEMORY", "PURGE"), b"OK")

        slow_help = b" ".join(self.cmd("SLOWLOG", "HELP")).lower()
        self.assertIn(b"get", slow_help)
        self.assertEqual(self.cmd("CONFIG", "SET", "slowlog-log-slower-than", "1"),
                         b"OK")
        self.cmd("DEBUG", "SLEEP", "0.05")
        self.assertGreaterEqual(self.cmd("SLOWLOG", "LEN"), 1)
        entries = self.cmd("SLOWLOG", "GET", "1")
        self.assertEqual(len(entries), 1)
        # Raise the threshold before RESET so RESET itself is not re-logged.
        self.assertEqual(self.cmd("CONFIG", "SET", "slowlog-log-slower-than",
                                  "10000"), b"OK")
        self.assertEqual(self.cmd("SLOWLOG", "RESET"), b"OK")
        self.assertEqual(self.cmd("SLOWLOG", "LEN"), 0)

        # RESETSTAT already covered by errorstats; keep a smoke check here.
        self.assertEqual(self.cmd("CONFIG", "RESETSTAT"), b"OK")

    def test_dump_restore_options(self):
        src = self.key("dump:src")
        dst = self.key("dump:dst")
        occupied = self.key("dump:busy")
        self.cmd("DEL", src, dst, occupied)
        self.cmd("SET", src, "payload")
        self.cmd("PEXPIRE", src, "60000")
        dump = self.cmd("DUMP", src)
        self.assertIsInstance(dump, (bytes, bytearray))
        self.assertTrue(dump)

        self.assertEqual(self.cmd("RESTORE", dst, "0", dump), b"OK")
        self.assertEqual(self.cmd("GET", dst), b"payload")

        self.cmd("SET", occupied, "old")
        with self.assertRaises(RedisError):
            self.cmd("RESTORE", occupied, "0", dump)
        self.assertEqual(self.cmd("RESTORE", occupied, "0", dump, "REPLACE"),
                         b"OK")
        self.assertEqual(self.cmd("GET", occupied), b"payload")

        # Absolute deadline in the past deletes immediately when ABSTTL is set.
        past = self.now_ms() - 1000
        gone = self.key("dump:past")
        self.cmd("DEL", gone)
        self.assertEqual(self.cmd("RESTORE", gone, str(past), dump, "ABSTTL"),
                         b"OK")
        self.assertEqual(self.cmd("EXISTS", gone), 0)

        future = self.key("dump:future")
        self.cmd("DEL", future)
        deadline = self.now_ms() + 120000
        self.assertEqual(
            self.cmd("RESTORE", future, str(deadline), dump, "ABSTTL"), b"OK")
        self.assert_ttl(future, low=1000, high=180000)

        idle = self.key("dump:idle")
        self.cmd("DEL", idle)
        self.assertEqual(
            self.cmd("RESTORE", idle, "0", dump, "IDLETIME", "7"), b"OK")
        self.assertGreaterEqual(self.cmd("OBJECT", "IDLETIME", idle), 7)

        with self.assertRaises(RedisError):
            self.cmd("RESTORE", self.key("dump:bad"), "0", dump,
                     "IDLETIME", "1", "FREQ", "1")
        with self.assertRaises(RedisError):
            self.cmd("RESTORE", self.key("dump:badfreq"), "0", dump,
                     "FREQ", "999")

    def test_client_reply_and_unblock(self):
        if self.server is None:
            self.skipTest("CLIENT REPLY/UNBLOCK tests require --server")

        quiet = self.connect()
        self.addCleanup(quiet.close)
        # REPLY OFF itself produces no reply; send raw and probe with select.
        quiet.sock.sendall(quiet.encode(("CLIENT", "REPLY", "OFF")))
        time.sleep(0.05)
        readable, _, _ = select.select([quiet.sock], [], [], 0.2)
        self.assertEqual(readable, [])
        quiet.sock.sendall(quiet.encode(("PING",)))
        time.sleep(0.05)
        readable, _, _ = select.select([quiet.sock], [], [], 0.2)
        self.assertEqual(readable, [])
        # ON restores replies and itself answers OK.
        quiet.sock.sendall(quiet.encode(("CLIENT", "REPLY", "ON")))
        self.assertEqual(quiet._read(), b"OK")
        self.assertEqual(quiet.command("PING"), b"PONG")

        skip = self.connect()
        self.addCleanup(skip.close)
        skip.sock.sendall(skip.encode(("CLIENT", "REPLY", "SKIP")))
        time.sleep(0.05)
        readable, _, _ = select.select([skip.sock], [], [], 0.2)
        self.assertEqual(readable, [])
        skip.sock.sendall(skip.encode(("PING",)))  # skipped
        time.sleep(0.05)
        readable, _, _ = select.select([skip.sock], [], [], 0.2)
        self.assertEqual(readable, [])
        self.assertEqual(skip.command("ECHO", "after-skip"), b"after-skip")

        blocker = self.connect()
        self.addCleanup(blocker.close)
        blocker_id = blocker.command("CLIENT", "ID")
        blkey = self.key("unblock:list")
        self.cmd("DEL", blkey)
        blocker.sock.sendall(blocker.encode(("BLPOP", blkey, "5")))
        time.sleep(0.1)
        self.assertEqual(self.cmd("CLIENT", "UNBLOCK", str(blocker_id)), 1)
        self.assertIsNone(blocker._read())
        self.assertEqual(self.cmd("CLIENT", "UNBLOCK", str(blocker_id)), 0)

        blocker2 = self.connect()
        self.addCleanup(blocker2.close)
        blocker2_id = blocker2.command("CLIENT", "ID")
        blocker2.sock.sendall(blocker2.encode(("BLPOP", blkey, "5")))
        time.sleep(0.1)
        self.assertEqual(
            self.cmd("CLIENT", "UNBLOCK", str(blocker2_id), "ERROR"), 1)
        err = blocker2._read()
        self.assertIsInstance(err, RedisError)
        self.assertIn("UNBLOCKED", str(err).upper())

    def test_object_and_pubsub_help(self):
        key = self.key("object:help")
        self.cmd("SET", key, "v")
        help_txt = b" ".join(self.cmd("OBJECT", "HELP")).lower()
        self.assertIn(b"encoding", help_txt)
        self.assertIn(b"idletime", help_txt)
        self.assertEqual(self.cmd("OBJECT", "ENCODING", key), b"embstr")
        self.assertGreaterEqual(self.cmd("OBJECT", "REFCOUNT", key), 1)
        # Default maxmemory-policy is noeviction / allkeys-lru style without LFU,
        # so FREQ should error while IDLETIME works.
        with self.assertRaises(RedisError):
            self.cmd("OBJECT", "FREQ", key)
        self.assertGreaterEqual(self.cmd("OBJECT", "IDLETIME", key), 0)

        pub_help = b" ".join(self.cmd("PUBSUB", "HELP")).lower()
        self.assertIn(b"channels", pub_help)
        self.assertIn(b"numsub", pub_help)
        self.assertIn(b"shardchannels", pub_help)

        if self.server is None:
            return
        sub = self.connect()
        self.addCleanup(sub.close)
        channel = self.key("pubsub:ch").decode("ascii")
        self.assertEqual(sub.command("SUBSCRIBE", channel)[0], b"subscribe")
        self.assertEqual(self.cmd("PUBSUB", "NUMSUB", channel),
                         [channel.encode("ascii"), 1])
        channels = self.cmd("PUBSUB", "CHANNELS", channel)
        self.assertIn(channel.encode("ascii"), channels)
        self.assertGreaterEqual(self.cmd("PUBSUB", "NUMPAT"), 0)

    def test_unlink_swapdb_wait_and_role(self):
        # UNLINK always lazy-frees; returns the number of deleted keys.
        k1 = self.key("unlink:a")
        k2 = self.key("unlink:b")
        missing = self.key("unlink:missing")
        self.cmd("SET", k1, "1")
        self.cmd("SET", k2, "2")
        self.assertEqual(self.cmd("UNLINK", k1, missing, k2), 2)
        self.assertIsNone(self.cmd("GET", k1))
        self.assertIsNone(self.cmd("GET", k2))
        self.assertEqual(self.cmd("UNLINK", missing), 0)

        # SWAPDB exchanges dict/expires of two DBs; clients keep their SELECT.
        a = self.key("swap:a")
        b = self.key("swap:b")
        self.cmd("SELECT", "0")
        self.cmd("SET", a, "db0")
        self.cmd("SELECT", "1")
        self.cmd("SET", b, "db1")
        self.cmd("SELECT", "0")
        self.assertEqual(self.cmd("SWAPDB", "0", "1"), b"OK")
        self.assertIsNone(self.cmd("GET", a))
        self.assertEqual(self.cmd("GET", b), b"db1")
        self.cmd("SELECT", "1")
        self.assertEqual(self.cmd("GET", a), b"db0")
        self.assertIsNone(self.cmd("GET", b))
        # Swap back so cleanup (DEL on SELECT 0) can remove both keys.
        self.cmd("SELECT", "0")
        self.assertEqual(self.cmd("SWAPDB", "0", "1"), b"OK")
        self.assertEqual(self.cmd("GET", a), b"db0")
        self.cmd("SELECT", "1")
        self.assertEqual(self.cmd("GET", b), b"db1")
        self.cmd("DEL", b)
        self.cmd("SELECT", "0")
        with self.assertRaises(RedisError):
            self.cmd("SWAPDB", "-1", "0")
        with self.assertRaises(RedisError):
            self.cmd("SWAPDB", "0", "999")

        # Standalone ROLE reports master with offset and empty slave list.
        role = self.cmd("ROLE")
        self.assertEqual(role[0], b"master")
        self.assertIsInstance(role[1], int)
        self.assertEqual(role[2], [])

        # Without replicas WAIT returns 0 immediately (including timeout 0).
        self.cmd("SET", self.key("wait:probe"), "1")
        self.assertEqual(self.cmd("WAIT", "1", "100"), 0)
        self.assertEqual(self.cmd("WAIT", "0", "0"), 0)
        with self.assertRaises(RedisError):
            self.cmd("WAIT", "1", "not-a-timeout")


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
