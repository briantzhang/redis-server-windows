# 运维手册

适用版本：Redis for Windows `5.0.14.1-bp16`，仅 Windows x64，单机、非集群。没有 Windows ARM64 构建。底座是 tporadowski/redis `v5.0.14.1`。

本文只覆盖这一个可执行文件的日常操作。它不包含 TLS、多段 AOF 和集群，那些配置项写进 `redis.conf` 也不会生效，启动时会报未知配置。ACL 已按 Redis 6 的范围实现，见第 3 节。

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

应看到 `v=5.0.14.1-bp16`。如果不是这个版本号，说明运行的不是这份回移。

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

- `bind 127.0.0.1` 只允许本机连接。要给别的机器用，改成具体网卡地址，并设口令。`requirepass` 仍然有效，它现在是 `default` 用户的口令；需要多个用户时用下面的 ACL。
- `appendfsync everysec` 最多丢一秒的写入。改成 `always` 每个写命令都刷盘，安全但慢；`no` 交给操作系统，断电时丢失不可控。
- `dir` 用正斜杠。改这个目录之前先 `SHUTDOWN`，否则 RDB 和 AOF 会写到旧目录。
- 内存上限按机器实际可用内存留余量，Windows 上吃满内存会连系统一起被拖慢。

改完配置必须重启进程才生效，本版没有运行期重载全部配置的能力。

### ACL 用户

本版的 ACL 是 Redis 6 的范围：每个用户只有一套权限。规则在运行期用命令设置，不会写进 `redis.conf`，重启后只剩 `default` 用户，其余要在启动脚本里重新下发。

```text
ACL SETUSER reader on >secret ~cache:* -@all +get +ping
AUTH reader secret
ACL WHOAMI
ACL CAT                  -- 列出全部分类
ACL CAT read             -- 列出 read 分类里的命令
ACL USERS                -- 列出全部用户
ACL GETUSER reader       -- 查看某个用户的标志、口令摘要、命令和键规则
ACL GENPASS 64           -- 生成 64 位（16 个十六进制字符）的随机口令
ACL DRYRUN reader get cache:1   -- 预检权限，不执行命令
```

- `on`/`off` 启用或停用用户。`>口令` 追加一个口令，口令以 SHA1 摘要保存，不明文落盘。
- `-@all +get +ping` 先清空命令权限再放行单个命令。分类有 `keyspace`、`read`、`write`、`admin`、`pubsub`、`fast`、`slow`，顺序有意义，`+get` 必须写在 `-@all` 后面。
- `~cache:*` 只允许匹配该通配符的键，`~*` 是全部键。键权限只有允许或不允许，没有 Redis 7 那种按键区分读和写的选择器，写了 `%R~` 这类规则会报未知规则。
- 一个用户不能删掉自己看不到的用户：`ACL` 命令本身受权限控制，但 `ACL WHOAMI` 对任何已认证用户都放行。
- 配置了 `requirepass` 时，未认证连接收到 `NOAUTH`；没有配置时，连接以 `default` 用户进入，`default` 默认拥有全部权限。
- `ACL DELUSER default` 会被拒绝。删掉一个用户后，仍以该用户身份在线的连接会立即被断开。
- `ACL LOG [条数]` 查看最近的拒绝记录（命令、键、认证失败），相同条目在 60 秒内合并计数；`ACL LOG RESET` 清空。`acllog-max-len` 控制条数上限，设为 0 关闭记录。
- 需要重启后保留用户时，在配置里设置 `aclfile <路径>`，文件每行一个 `user <名称> <规则...>`。启动时自动加载；运行期用 `ACL SAVE` 写回、`ACL LOAD` 重新加载。加载失败时保留内存中的旧用户并返回错误。`aclfile` 不能在运行期用 `CONFIG SET` 修改。ACL 文件里的 `default` 行要带上 `&*`（或 `allchannels`），否则默认用户不能订阅和发布。
- 频道权限用 `&pattern` 设置，例如 `ACL SETUSER reader on >secret ~cache:* &news:* +@all`。字面频道按通配符匹配，`PSUBSCRIBE` 的模式必须与 ACL 模式字面一致。新建用户默认没有任何频道权限，`default` 用户默认 `allchannels`。`&*` 与 `allchannels` 等价，`resetchannels` 清空。已经在 `allchannels` 状态时再添加 `&pattern` 会报错，需要先 `resetchannels`。
- 脚本里的 `redis.call` 也按调用者的用户检查命令、键和频道；越权时脚本直接报错。

### 大键删除与清库

`DEL` 和 `FLUSHDB`/`FLUSHALL` 默认在主线程同步释放内存，删除几百万元素的键时会卡住事件循环。本版补了 Redis 6 的两个开关：

```conf
lazyfree-lazy-user-del yes
lazyfree-lazy-user-flush yes
```

打开后 `DEL` 等价于 `UNLINK`，不带 `ASYNC` 的清库也走后台释放。代价是内存不会立刻归还，`INFO memory` 里的 `lazyfree_pending_objects` 会短暂大于 0。也可以不改配置，只在具体命令上使用 `UNLINK` 和 `FLUSHDB ASYNC`。

## 4. 新增命令怎么用

这些命令是本版相对底座新增的，语义按 Redis 7.0 单机行为实现。

保留 TTL 覆盖值：

```text
SET session:1 "{\"uid\":1}" EX 3600
SET session:1 "{\"uid\":1,\"v\":2}" KEEPTTL
TTL session:1
```

第二次写入后剩余时间应与第一次接近，而不是被清掉。不带 `KEEPTTL` 的 `SET` 会清除原有过期时间，这是底座的原行为。

列表查找与原子移动：

```text
RPUSH queue a b c b
LPOS queue b                  -- 1
LPOS queue b COUNT 0          -- 1 3
LPOS queue b RANK -1          -- 3
LMOVE queue other LEFT RIGHT  -- 取出 a 放到 other 尾部
```

`LPOS` 的 `MAXLEN` 限制最多扫描多少个元素，适合在大列表上做有界查找。`LMOVE` 的源和目标可以是同一个列表，用来做轮转。

只算交集大小，不返回元素：

```text
SINTERCARD 2 set:a set:b
SINTERCARD 2 set:a set:b LIMIT 100
ZINTERCARD 2 zset:a zset:b
```

`LIMIT` 只影响何时提前结束，不影响结果值：命中数达到上限就立即返回该上限。

条件更新分数与随机取样：

```text
ZADD board GT CH 15 amy
ZADD board INCR GT 5 amy
HRANDFIELD profile 3 WITHVALUES
HRANDFIELD profile -10
ZRANDMEMBER board 2 WITHSCORES
ZRANDMEMBER board -5
```

`GT`/`LT` 只约束已有成员的分数更新，新成员照常加入；与 `NX` 或彼此同时出现会报错。`INCR` 在条件不满足时返回空。`HRANDFIELD`/`ZRANDMEMBER` 的 `count` 为正数时返回不重复的元素，超过集合大小时返回全部；为负数时允许重复，绝对值是返回个数；不带 `count` 时返回单个元素。

批量弹出与阻塞移动：

```text
LPOP queue 10
RPOP queue 10
BLMOVE queue processing LEFT RIGHT 5
BLMOVE queue processing LEFT RIGHT 0.5
```

`LPOP`/`RPOP` 带 `count` 时返回数组，列表不足时返回剩余的全部元素，清空后键被删除；不带 `count` 时仍是原来的单元素回复。`BLMOVE` 在源列表为空时阻塞，超时返回空，超时参数支持小数秒。

统一范围查询与范围落库：

```text
ZRANGE board 0 -1 WITHSCORES
ZRANGE board -inf +inf BYSCORE LIMIT 0 10
ZRANGE board 100 50 BYSCORE REV
ZRANGE board [a [z BYLEX
ZRANGESTORE top10 board 0 9 REV
```

`BYSCORE` 的区间支持 `(` 开区间和 `-inf`/`+inf`，`BYLEX` 使用 `[`、`(`、`-`、`+`；`LIMIT` 只能和 `BYSCORE`/`BYLEX` 一起用，`WITHSCORES` 不能和 `BYLEX` 一起用。`ZRANGESTORE` 没有 `WITHSCORES`，结果为空时会删除目标键并返回 0。原来的 `ZRANGEBYSCORE`、`ZREVRANGEBYLEX` 等命令保持不变，内部走同一套实现。

多键批量弹出：

```text
LMPOP 2 queue:high queue:low LEFT COUNT 10
BLMPOP 5 2 queue:high queue:low RIGHT COUNT 10
ZMPOP 2 board:hot board:cold MAX COUNT 5
BZMPOP 5 2 board:hot board:cold MIN COUNT 5
```

`LMPOP`/`BLMPOP` 依次检查键，从第一个非空列表弹出，返回 `[键, [元素 ...]]`；所有键都为空时返回空数组。`ZMPOP`/`BZMPOP` 同理，返回 `[键, [[成员, 分数], ...]]`。`COUNT` 必须大于 0，缺省为 1；弹出后列表或有序集合为空会删除键。阻塞版本的超时支持小数秒，在 `MULTI/EXEC` 中不阻塞、立即返回空。复制和 AOF 里记录的是 `LPOP`/`RPOP`/`ZPOPMIN`/`ZPOPMAX` 加 `COUNT`，不是原来的多键命令。

客户端标志：

```text
CLIENT NO-TOUCH on
CLIENT NO-TOUCH off
CLIENT NO-EVICT on
```

`CLIENT NO-TOUCH` 让这个连接后续读取键时不再更新 LRU/LFU，`TOUCH` 命令本身仍会更新；`OBJECT IDLETIME`/`OBJECT FREQ` 可以用来核对。`CLIENT NO-EVICT` 只记录标志：本版没有实现 Redis 7 的客户端内存驱逐（`maxmemory-clients`），因此不会真的因为内存压力保留或驱逐某个连接。`RESET` 会清除这两个标志。

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

位范围与只读命令：

```text
SET bitmap "abc"
BITCOUNT bitmap 0 0 BIT          -- 只看第 0 位
BITCOUNT bitmap 16 23 BIT        -- 只看第 3 个字节
BITPOS bitmap 0 2 -1             -- 从第 2 个字节开始找 0 位
BITFIELD_RO bitmap GET u8 0      -- 只读读取，SET/INCRBY 会报错
BITCOUNT bitmap 0 0 BIT          -- 只看第 0 位
BITCOUNT bitmap 16 23 BIT        -- 只看第 3 个字节
BITPOS bitmap 0 2 -1             -- 从第 2 个字节开始找 0 位
BITFIELD_RO bitmap GET u8 0      -- 只读读取，SET/INCRBY 会报错
LCS key1 key2 LEN                -- 最长公共子序列长度
LCS key1 key2 IDX MINMATCHLEN 4  -- 输出匹配区间
SORT_RO list LIMIT 0 10          -- 带 STORE 会报错
EVAL_RO "return redis.call('get',KEYS[1])" 1 k
```

`EVAL_RO`/`EVALSHA_RO` 只允许脚本调用读命令；脚本里出现写命令时整个调用报错，数据不变。需要写操作请继续用 `EVAL`/`EVALSHA`。

暂停与恢复：

```text
CLIENT PAUSE 5000 WRITE   -- 5 秒内只拦写命令，读命令照常
CLIENT PAUSE 5000 ALL     -- 读写都拦
CLIENT UNPAUSE            -- 立即恢复，不等超时
CLIENT INFO               -- 当前连接的信息，和 CLIENT LIST 的单行格式相同
```

暂停只影响普通客户端，副本连接不受影响。暂停期间到达的命令会先解析并挂起，恢复后按到达顺序执行；客户端不需要重发。

配置与命令清单：

```text
CONFIG GET maxmemory maxmemory-samples
CONFIG SET maxmemory-samples 7 timeout 123
COMMAND LIST FILTERBY PATTERN bit*
COMMAND LIST FILTERBY ACLCAT write
COMMAND GETKEYSANDFLAGS MSET a 1 b 2
```

`CONFIG GET` 对重叠的模式去重。`CONFIG SET` 逐对应用：如果第二对参数非法，第一对已经生效，返回错误后要自己检查当前值。`COMMAND GETKEYSANDFLAGS` 的键标志是按命令级读写属性近似出来的（`RO`/`RW`、`access`、`update`），没有 Redis 7 的细粒度键规格。

地理搜索与连接排查：

```text
GEOSEARCH places FROMLONLAT 116.40 39.90 BYRADIUS 10 km ASC WITHDIST
GEOSEARCHSTORE nearby places FROMMEMBER home BYBOX 20 20 km STOREDIST
CLIENT LIST                       -- 每行现在带 laddr= 和 user=
CLIENT KILL USER reader           -- 断开某个 ACL 用户的全部连接
CLIENT KILL MAXAGE 3600           -- 断开建立超过一小时的连接
CLIENT KILL LADDR 127.0.0.1:6379  -- 命中该监听地址上的全部连接
```

`CLIENT KILL` 默认跳过发起命令的连接；`LADDR` 是服务端本地地址，不是客户端地址，所以同一监听端口上的连接都会命中。`MAXAGE` 按连接建立至今的秒数计算，`-1` 会被拒绝。`INFO everything` 与 `INFO all` 输出相同；`INFO errorstats` 按错误码给出计数，`CONFIG RESETSTAT` 会清零。客户端库可以用 `CLIENT SETINFO lib-name <名称>` 和 `CLIENT SETINFO lib-ver <版本>` 登记自己，`CLIENT LIST` 的 `lib-name=`、`lib-ver=` 会显示出来。

命令延迟统计：

```text
LATENCY HISTOGRAM              -- 所有有样本的命令
LATENCY HISTOGRAM get set      -- 只看指定命令
INFO latencystats              -- 每条命令的 p50/p99/p99.9（微秒）
LATENCY RESET                  -- 清空事件与直方图
CONFIG SET latency-tracking no -- 停止采集，已有数据保留
```

直方图按 1024ns 起、2 的幂分桶，`histogram_usec` 是“桶上界 → 累计次数”的扁平数组。它适合判断延迟量级，不是精确的尾延迟分布。直方图只存在内存里，重启或 `CONFIG RESETSTAT` 后从零开始。

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

需要确认 AOF 已经落盘时：

```text
WAITAOF 1 0 1000
```

本版在命令内同步刷盘后立即返回 `[1, 0]`；第二项是副本 AOF 确认数，始终为 0。它不会像 Redis 7.2 那样阻塞到超时，也不能用来等待副本。`SHUTDOWN` 另接受 `NOW`（不等副本）、`FORCE`（保存失败也退出）和 `ABORT`（取消已置位但尚未执行的关闭请求）；正常停机仍建议先 `BGSAVE`/`BGREWRITEAOF`，确认完成后再 `SHUTDOWN`。

## 6. 备份

备份前先拿到一致的文件：

```text
BGSAVE
BGREWRITEAOF
```

等 `INFO persistence` 里两次后台任务的状态都变成 `ok`，再复制 `dir` 下的 `dump.rdb` 和 `appendonly.aof`。复制过程中不要 `SHUTDOWN`，也不要再手动触发保存。

备份文件名带上时间和版本，例如 `dump-5.0.14.1-bp16-20261109.rdb`，和程序分开存放。恢复演练至少做一次：把备份放到一个新目录、用另一个端口起一个临时实例读出来核对，不要在生产端口上试。

流的 `entries-added` 和 `max-deleted-entry-id` 不写入 RDB 快照：从 `dump.rdb` 恢复后前者按当前长度重算、后者归零；从 AOF 恢复（含 AOF 重写）会保留。若业务依赖这两个计数，恢复时优先使用 AOF，或在恢复后用 `XSETID <key> <id> ENTRIESADDED <n> MAXDELETEDID <id>` 校正。

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
- 本版已修复一处 AOF 写入崩溃：带过期参数的 `SET`（如 `SET k v PX 180000`）在旧二进制上会触发访问冲突。确认 `redis-server --version` 是 `5.0.14.1-bp1` 或之后的版本，且构建提交为 `71377016` 之后的版本。

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
- 客户端如果依赖 Redis 6 以上的 `HELLO 3`（RESP3）会协商失败，应使用 RESP2。本版 `CLIENT TRACKING` 可用，但失效通知走 Pub/Sub 频道 `__redis__:invalidate`（需 `REDIRECT` 到已订阅该频道的连接），没有 RESP3 push。Redisson 单机模式下的 `SET ... KEEPTTL`、`GETEX`、`GETDEL` 属于本版已实现的范围。
