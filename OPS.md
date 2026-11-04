# 运维手册

适用版本：Redis for Windows `5.0.14.1-bp1`，仅 Windows x64，单机、非集群。没有 Windows ARM64 构建。底座是 tporadowski/redis `v5.0.14.1`。

本文只覆盖这一个可执行文件的日常操作。它不包含 Redis 官方 6/7/8 才有的 ACL、TLS、多段 AOF 和集群，那些配置项写进 `redis.conf` 也不会生效，启动时会报未知配置。

## 1. 目录与文件

编译产物在 `msvs\x64\Release`：

| 文件 | 用途 |
| --- | --- |
| `redis-server.exe` | 服务进程 |
| `redis-cli.exe` | 命令行客户端 |
| `redis-benchmark.exe` | 压测工具，不要对生产实例跑 |

运行时数据默认写在启动时的工作目录：

| 文件 | 来源 | 说明 |
| --- | --- | --- |
| `dump.rdb` | `dbfilename` | RDB 快照 |
| `appendonly.aof` | `appendfilename` | 单文件 AOF，默认关闭 |
| 日志 | `logfile` | 默认空，输出到控制台 |

部署时把这三个路径指到独立的数据盘目录，不要和程序目录混在一起。

## 2. 启动与停止

前台启动，便于第一次确认配置能被读入：

```powershell
.\redis-server.exe .\redis.conf
```

确认版本：

```powershell
.\redis-server.exe --version
```

应看到 `v=5.0.14.1-bp1`。如果不是这个版本号，说明运行的不是这份回移。

另开一个窗口检查存活：

```powershell
.\redis-cli.exe -p 6379 PING
```

返回 `PONG` 即正常。

停止用客户端命令，让它把 AOF 刷盘后再退出：

```powershell
.\redis-cli.exe -p 6379 SHUTDOWN
```

不要直接结束进程。直接结束会丢掉还在缓冲区里的写入，AOF 和 RDB 都可能停在上一次刷盘。

注册成 Windows 服务沿用底座的参数，服务名自定：

```powershell
.\redis-server.exe --service-install .\redis.conf --service-name RedisBp1
.\redis-server.exe --service-start --service-name RedisBp1
```

停止和卸载：

```powershell
.\redis-server.exe --service-stop --service-name RedisBp1
.\redis-server.exe --service-uninstall --service-name RedisBp1
```

## 3. 建议的最小配置

下面是单机、只本机访问、开启 AOF 的起点。按实际端口、目录和内存修改。

```conf
bind 127.0.0.1
port 6379
protected-mode yes
databases 16

dir D:/redis-data
dbfilename dump.rdb

save 900 1
save 300 10
save 60 10000

appendonly yes
appendfilename appendonly.aof
appendfsync everysec
auto-aof-rewrite-percentage 100
auto-aof-rewrite-min-size 64mb

maxmemory 2gb
maxmemory-policy allkeys-lru

logfile D:/redis-data/redis.log
```

几点说明：

- `bind 127.0.0.1` 只允许本机连接。要给别的机器用，改成具体网卡地址，并设 `requirepass`。底座的口令是明文配置项，没有 ACL 用户体系。
- `appendfsync everysec` 最多丢一秒的写入。改成 `always` 每个写命令都刷盘，安全但慢；`no` 交给操作系统，断电时丢失不可控。
- `dir` 用正斜杠。改这个目录之前先 `SHUTDOWN`，否则 RDB 和 AOF 会写到旧目录。
- 内存上限按机器实际可用内存留余量，Windows 上吃满内存会连系统一起被拖慢。

改完配置必须重启进程才生效，本版没有运行期重载全部配置的能力。

## 4. 新增命令怎么用

这些命令是本版相对底座新增的，语义按 Redis 7.0 单机行为实现。

保留 TTL 覆盖值：

```text
SET session:1 "{\"uid\":1}" EX 3600
SET session:1 "{\"uid\":1,\"v\":2}" KEEPTTL
TTL session:1
```

第二次写入后剩余时间应与第一次接近，而不是被清掉。不带 `KEEPTTL` 的 `SET` 会清除原有过期时间，这是底座的原行为。

原子取出旧值并写入：

```text
SET counter 1
SET counter 2 GET
```

返回旧值 `1`，键的新值是 `2`。键不存在时返回空。

读取并删除：

```text
SET job:1 payload
GETDEL job:1
GET job:1
```

`GETDEL` 返回 `payload`，之后键不存在。两个客户端同时 `GETDEL` 同一个键，只有一个拿得到值。

读取并改 TTL：

```text
SET cache:1 value
GETEX cache:1 EX 60
GETEX cache:1 PERSIST
```

第一条把 TTL 设为 60 秒并返回值，第二条去掉过期时间。

条件更新过期时间：

```text
SET k v EX 100
EXPIRE k 50 GT
EXPIRE k 200 XX
```

`GT` 只在新 TTL 大于现有 TTL 时生效，所以上面第一条返回 `0`。`XX` 只在键已有过期时间时生效。`NX` 与 `XX`、`GT` 与 `LT` 同时出现会报错，键保持原状。

查绝对过期时间：

```text
PEXPIRETIME k
EXPIRETIME k
```

返回 Unix 时间戳，前者毫秒、后者秒。键不存在返回 `-2`，键存在但没有过期时间返回 `-1`。

批量查询：

```text
SADD tags a b
SMISMEMBER tags a c b
ZADD board 10 amy 20 bob
ZMSCORE board amy carl
```

`SMISMEMBER` 按参数顺序返回 `1/0`。`ZMSCORE` 对不存在的成员返回空。

## 5. 持久化与恢复

RDB 是某个时间点的快照，AOF 是写命令日志。两者同时开启时，重启优先用 AOF 恢复，因为它更接近停机前的状态。

手动触发一次快照：

```text
BGSAVE
```

查看最近一次保存是否在进行、AOF 大小和重写状态：

```text
INFO persistence
```

关注 `rdb_last_bgsave_status`、`aof_last_bgrewrite_status` 是否为 `ok`，以及 `aof_current_size` 是否在重写后回落。

AOF 重写（压缩日志、用当前数据重放生成新文件）：

```text
BGREWRITEAOF
```

重写期间写入会先进重写缓冲区，完成后再追加，不需要停机。本版是底座的单文件 AOF，没有 Redis 7 的多段 AOF，所以重写是整文件替换。

恢复步骤：

1. `SHUTDOWN` 停掉实例，确认进程已退出。
2. 把整个 `dir` 目录拷走一份，作为回退点。
3. 用要恢复的 `dump.rdb` 和 `appendonly.aof` 覆盖数据目录里的同名文件。只恢复快照时，移走 `appendonly.aof`，否则 AOF 会覆盖 RDB 的内容。
4. 启动后抽查键数量和关键业务键：`INFO keyspace`、`GET`、`TTL`。

本版把带过期时间的 `SET` 记成 `SET` 加绝对时间的 `PEXPIREAT`。所以从 AOF 恢复后，TTL 不会因为“重放时重新计算相对时间”而变长。

## 6. 备份

备份前先拿到一致的文件：

```text
BGSAVE
BGREWRITEAOF
```

等 `INFO persistence` 里两次后台任务的状态都变成 `ok`，再复制 `dir` 下的 `dump.rdb` 和 `appendonly.aof`。复制过程中不要 `SHUTDOWN`，也不要再手动触发保存。

备份文件名带上时间和版本，例如 `dump-5.0.14.1-bp1-20261104.rdb`，和程序分开存放。恢复演练至少做一次：把备份放到一个新目录、用另一个端口起一个临时实例读出来核对，不要在生产端口上试。

## 7. 排查

连不上：

- `redis-cli -p <端口> PING` 是否返回 `PONG`。
- 端口是否被占用，配置里的 `port` 和客户端是否一致。
- `bind` 是否只监听了 `127.0.0.1`，远端连接会被拒绝。
- 设了 `requirepass` 时客户端要先 `AUTH`。

启动后立刻退出：

- 看 `logfile`。最常见的是配置项拼写错误、`dir` 不存在或没有写权限。
- 数据目录里残留一把锁或损坏的 AOF 时，日志会给出具体文件。先把该目录整体拷贝出来再处理，不要直接删。

AOF 启动失败：

- 保留现场后，用底座自带的方式检查：`redis-check-aof --fix` 会截掉文件末尾不完整的最后一条命令。只在确认丢失末尾这一条可以接受时使用，并先备份原文件。
- 本版已修复一处 AOF 写入崩溃：带过期参数的 `SET`（如 `SET k v PX 180000`）在旧二进制上会触发访问冲突。确认 `redis-server --version` 是 `5.0.14.1-bp1` 且构建提交为 `a7544cf5` 之后的版本。

内存涨个不停：

- `INFO memory` 看 `used_memory_human`。
- 没有 `maxmemory` 时 Redis 不会主动淘汰。按第 3 节设置上限和淘汰策略。
- `INFO keyspace` 看 `expires` 是否远小于 `keys`，大量不设 TTL 的键会一直留着。

## 8. 从旧的 5.0.14.1 升级

1. 对旧实例执行 `BGSAVE`，确认完成后复制 `dump.rdb`。
2. `SHUTDOWN` 旧实例。
3. 用本版 `redis-server.exe` 指向同一个 `dir` 启动。RDB 格式没有变，可以直接读。
4. 抽查键数量、若干业务键的值和 TTL。

回退就是反向操作：停掉本版，把备份的 `dump.rdb` 放回，用旧的 `redis-server.exe` 启动。不要用本版生成的 AOF 去喂旧版本，旧版本不认识回移后的写法。

## 9. 不在本版范围内的事

- 不支持把一份数据目录挂给两个进程同时写。
- 没有主从复制之外的高可用。底座保留了原有的复制命令，但本版没有额外验证哨兵和集群，生产不要靠它做故障切换。
- 没有访问控制列表。需要账号级别的权限隔离时，本版做不到。
- 客户端如果依赖 Redis 6 以上的 `HELLO 3`（RESP3）会协商失败，应使用 RESP2。Redisson 单机模式下的 `SET ... KEEPTTL`、`GETEX`、`GETDEL` 属于本版已实现的范围。
