#!/usr/bin/env python3
"""Package the tested Windows backport; never modifies an installed service."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '5.0.14.1-bp17'
NAME = 'Redis-x64-' + VERSION
DIST = ROOT / 'dist'
PKG = DIST / NAME
BUILD = ROOT / 'msvs' / 'x64' / 'Release'
BINARIES = ['redis-server.exe', 'redis-cli.exe', 'redis-benchmark.exe',
            'redis-check-rdb.exe', 'redis-check-aof.exe']


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    DIST.mkdir(exist_ok=True)
    # A distinct release directory prevents mixing cached old binaries.
    if PKG.exists():
        raise SystemExit('Release directory already exists: ' + str(PKG))
    for name in BINARIES:
        if not (BUILD/name).is_file():
            raise SystemExit('Missing build output: ' + str(BUILD/name))
    version = subprocess.check_output([str(BUILD/'redis-server.exe'), '--version'], text=True)
    if VERSION not in version:
        raise SystemExit('Unexpected redis-server version: ' + version)
    test_log = ROOT/'build-tools'/'regression-windows.log'
    if not test_log.exists() or not test_log.read_text(encoding='utf-8').rstrip().endswith('OK'):
        raise SystemExit('A successful native regression log is required')
    PKG.mkdir()
    for name in BINARIES:
        shutil.copy2(BUILD/name, PKG/name)
    for name in ['COPYING', 'BACKPORT-RELEASE.md']:
        shutil.copy2(ROOT/name, PKG/name)
    licenses = PKG/'licenses'
    licenses.mkdir()
    for src, dest in [('deps/hiredis/COPYING','hiredis-COPYING'),
                      ('deps/jemalloc-5.2.1/COPYING','jemalloc-COPYING'),
                      ('deps/lua/COPYRIGHT','lua-COPYRIGHT')]:
        shutil.copy2(ROOT/src, licenses/dest)
    shutil.copy2(test_log, PKG/'regression-windows.log')
    for name in ['benchmark-windows.json', 'verification-windows.md']:
        if (DIST/name).exists():
            shutil.copy2(DIST/name, PKG/name)
    config = '''# Redis Windows backport. Local connections only by default.
bind 127.0.0.1
protected-mode yes
port 6379
tcp-backlog 511
timeout 0
tcp-keepalive 300
daemonize no
loglevel notice
logfile ""
databases 16
save 900 1
save 300 10
save 60 10000
stop-writes-on-bgsave-error yes
rdbcompression yes
rdbchecksum yes
dbfilename dump.rdb
dir ./data
appendonly yes
appendfilename appendonly.aof
appendfsync everysec
no-appendfsync-on-rewrite no
auto-aof-rewrite-percentage 100
auto-aof-rewrite-min-size 64mb
aof-load-truncated yes
aof-use-rdb-preamble yes
# Set a strong requirepass in this file before exposing to other hosts.
'''
    (PKG/'redis.windows.conf').write_text(config, encoding='ascii')
    (PKG/'start.cmd').write_text('''@echo off
setlocal
pushd "%~dp0"
if not exist data mkdir data
"%~dp0redis-server.exe" "%~dp0redis.windows.conf" --dir "%~dp0data"
popd
''', encoding='ascii', newline='\r\n')
    (PKG/'README.md').write_text('''# Redis Windows 5.0.14.1-bp17

Windows x64 原生免编译 ZIP。解压后双击 start.cmd，在前台运行；关闭窗口或 Ctrl+C 停止。
默认监听 127.0.0.1:6379，AOF 每秒同步，数据保存在同目录 data/。
确认没有其他 Redis 占用 6379；如需调整端口，编辑 redis.windows.conf。
这是自维护命令回移版，不是官方 Redis 6/7/8。具体命令、限制及来源见 BACKPORT-RELEASE.md。

检查版本：redis-server.exe --version
测试连接：redis-cli.exe -h 127.0.0.1 -p 6379 PING
新命令示例：
  redis-cli.exe SET demo value KEEPTTL
  redis-cli.exe ZUNION 2 z1 z2 WITHSCORES
  redis-cli.exe COPY src dst REPLACE
  redis-cli.exe ACL SETUSER reader on ">secret" "~cache:*" -@all +get +ping
  redis-cli.exe HRANDFIELD h 2 WITHVALUES
  redis-cli.exe ZRANDMEMBER z 2 WITHSCORES
  redis-cli.exe LPOP queue 10
  redis-cli.exe ZRANGE board -inf +inf BYSCORE LIMIT 0 10 WITHSCORES
  redis-cli.exe LMPOP 2 queue:high queue:low LEFT COUNT 10
  redis-cli.exe ZMPOP 2 board:hot board:cold MAX COUNT 5
  redis-cli.exe XADD events NOMKSTREAM MAXLEN ~ 1000 '*' type login
  redis-cli.exe XAUTOCLAIM jobs workers w1 60000 0-0 COUNT 10
  redis-cli.exe XINFO STREAM events FULL COUNT 5
  redis-cli.exe BITCOUNT bitmap 0 -1 BIT
  redis-cli.exe LCS key1 key2 LEN
  redis-cli.exe SORT_RO list LIMIT 0 10
  redis-cli.exe CLIENT PAUSE 5000 WRITE
  redis-cli.exe CONFIG GET maxmemory maxmemory-samples
  redis-cli.exe COMMAND LIST FILTERBY PATTERN bit*
  redis-cli.exe GEOSEARCH places FROMLONLAT 116.40 39.90 BYRADIUS 10 km ASC WITHDIST
  redis-cli.exe ACL CAT read
  redis-cli.exe CLIENT KILL USER reader
  redis-cli.exe WAITAOF 1 0 1000
  redis-cli.exe ACL LOG
  redis-cli.exe ACL SAVE
  redis-cli.exe CLIENT SETINFO lib-name myapp
  redis-cli.exe XGROUP SETID events g1 0 ENTRIESREAD 0
  redis-cli.exe INFO errorstats
  redis-cli.exe SSUBSCRIBE news
  redis-cli.exe LATENCY HISTOGRAM
  redis-cli.exe INFO latencystats
  redis-cli.exe SCRIPT FLUSH ASYNC
  redis-cli.exe COMMAND DOCS get zrank
  redis-cli.exe ZRANK z WITHSCORE
  redis-cli.exe ACL WHOAMI
  redis-cli.exe SCAN 0 TYPE string
  redis-cli.exe HELLO 2
  redis-cli.exe CLIENT TRACKING ON REDIRECT 1
  redis-cli.exe CLIENT TRACKING ON BCAST PREFIX cache:
  redis-cli.exe CONFIG GET tracking-table-max-keys
  redis-cli.exe INFO clients
  redis-cli.exe LATENCY DOCTOR
  redis-cli.exe MEMORY DOCTOR
  redis-cli.exe DUMP key
  redis-cli.exe CLIENT REPLY ON
  redis-cli.exe OBJECT ENCODING key
  redis-cli.exe PUBSUB HELP
  redis-cli.exe UNLINK bigkey
  redis-cli.exe SWAPDB 0 1
  redis-cli.exe WAIT 1 1000
  redis-cli.exe ROLE
  redis-cli.exe QUIT
  redis-cli.exe XINFO CONSUMERS mystream mygroup

本包不自动创建 Windows 服务、不开防火墙、不覆盖已有数据。部署为服务时可使用底座原有
--service-install 功能，并明确提供配置文件绝对路径及 --dir 数据目录绝对路径；先在目标机验证。
需要远程连接时由管理员配置监听地址、强密码与访问控制。

请保留 COPYING。回归结果见 regression-windows.log，构建与测试范围见 verification-windows.md。
''', encoding='utf-8')
    (PKG/'manifest.json').write_text(json.dumps({
        'version': VERSION, 'platform': 'windows-x64',
        'base_tag': 'v5.0.14.1',
        'base_commit': subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),
        'tracked_files_modified':
            bool(subprocess.check_output(['git','-C',str(ROOT),'diff','--name-only','HEAD'],
                                         text=True).strip()),
        'server_version': version.strip(),
    }, indent=2)+'\n', encoding='utf-8')
    records = [f'{digest(p)}  {p.relative_to(PKG).as_posix()}'
               for p in sorted(PKG.rglob('*')) if p.is_file()]
    (PKG/'checksums.sha256').write_text('\n'.join(records)+'\n', encoding='ascii')
    archive = DIST/(NAME+'.zip')
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as output:
        for p in sorted(PKG.rglob('*')):
            if p.is_file():
                output.write(p, p.relative_to(DIST))
    sources = DIST/(NAME+'-source.zip')
    tracked = subprocess.check_output(['git','-C',str(ROOT),'ls-files','-z']).decode().split('\0')
    extra = ['BACKPORT-RELEASE.md','tests/backport_regression.py','tests/backport_benchmark.py',
             'build-tools/build-windows.ps1','build-tools/package-windows.py',
             'build-tools/verify-native.py']
    with zipfile.ZipFile(sources,'w',zipfile.ZIP_DEFLATED) as output:
        for rel in sorted(set(tracked+extra)):
            if rel and (ROOT/rel).is_file():
                output.write(ROOT/rel, NAME+'-source/'+rel)
    sums = DIST/(NAME+'-archives.sha256')
    sums.write_text(''.join(f'{digest(p)}  {p.name}\n' for p in [archive,sources]), encoding='ascii', newline='\n')
    for p in [archive,sources,sums]:
        print(str(p), p.stat().st_size, flush=True)


if __name__ == '__main__':
    main()
