# Redis for Windows 5.0.14.1-bp14

Windows x64 上的单机 Redis，以 [tporadowski/redis](https://github.com/tporadowski/redis) 的
`v5.0.14.1`（提交 `ec77f72dc`，2022-02 发布）为底座，选择性回移了 Redis 6.0–7.2 的一批单机命令。

它不是 Redis 官方发行版，也不是 Redis 6、7 或 8。`redis-server --version` 报告的版本号是 `5.0.14.1-bp14`。

## 这个版本新增了什么

| 命令 | 上游引入版本 | 行为 |
| --- | --- | --- |
| `SET key value [NX\|XX] [GET] [EX\|PX\|EXAT\|PXAT time\|KEEPTTL]` | 6.0 / 6.2；NX+GET 取自 7.0 | 覆盖时可保留原 TTL，可原子返回旧值，可按绝对时间过期 |
| `GETEX key [EX\|PX\|EXAT\|PXAT time\|PERSIST]` | 6.2 | 原子读取并更新或去掉 TTL；不带选项时不改 TTL |
| `GETDEL key` | 6.2 | 原子读取并删除字符串 |
| `EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT` 的 `NX\|XX\|GT\|LT` | 7.0 | 按条件更新 TTL；允许 XX+GT、XX+LT，拒绝冲突组合 |
| `EXPIRETIME key` / `PEXPIRETIME key` | 7.0 | 查询绝对过期时间；键不存在返回 -2，没有 TTL 返回 -1 |
| `SMISMEMBER key member [member ...]` | 6.2 | 一次判断多个集合成员 |
| `ZMSCORE key member [member ...]` | 6.2 | 一次查询多个有序集合分数 |
| `ZUNION numkeys key [key ...] [WEIGHTS w ...] [AGGREGATE SUM\|MIN\|MAX] [WITHSCORES]` | 6.2 | 多键有序集合并集，只返回结果不落库 |
| `ZINTER numkeys key [key ...] [WEIGHTS w ...] [AGGREGATE SUM\|MIN\|MAX] [WITHSCORES]` | 6.2 | 多键有序集合交集 |
| `ZDIFF numkeys key [key ...] [WITHSCORES]` | 6.2 | 第一个键减去其余键的差集 |
| `ZDIFFSTORE destination numkeys key [key ...]` | 6.2 | 差集写入目标键 |
| `COPY source destination [DB db] [REPLACE]` | 6.2 | 深拷贝任意类型的键；不带 REPLACE 时不覆盖已存在的目标 |
| `RESET` | 6.2 | 连接回到 0 号库并清除客户端状态 |
| `LPOS key element [RANK r] [COUNT n] [MAXLEN m]` | 6.0 | 查找元素下标，可从尾部查找、限制返回个数与扫描长度 |
| `LMOVE source destination LEFT\|RIGHT LEFT\|RIGHT` | 6.2 | 原子地在两个列表间移动元素，源和目标可以是同一个列表 |
| `SINTERCARD numkeys key [key ...] [LIMIT n]` | 7.0 | 只返回交集基数，不构建结果集 |
| `ZINTERCARD numkeys key [key ...] [LIMIT n]` | 7.0 | 只返回有序集合交集基数 |
| `ZADD key [NX\|XX] [GT\|LT] [CH] [INCR] score member ...` 的 `GT\|LT` | 6.2 | 只在新分数严格大于/小于当前分数时更新已有成员 |
| `HRANDFIELD key [count [WITHVALUES]]` | 6.2 | 随机返回字段；正数去重、负数可重复、可带值 |
| `ZRANDMEMBER key [count [WITHSCORES]]` | 6.2 | 随机返回成员；正数去重、负数可重复、可带分数 |
| `LPOP key [count]` / `RPOP key [count]` | 6.2 | 批量弹出；不带 count 时仍是单元素回复 |
| `BLMOVE source destination LEFT\|RIGHT LEFT\|RIGHT timeout` | 6.2 | LMOVE 的阻塞版本，支持小数秒超时 |
| `ZRANGE key start stop [BYSCORE\|BYLEX] [REV] [LIMIT o c] [WITHSCORES]` | 6.2 | 统一的范围查询，旧命令仍可用 |
| `ZRANGESTORE dst src start stop [BYSCORE\|BYLEX] [REV] [LIMIT o c]` | 6.2 | 把范围结果写入目标键 |
| `LMPOP numkeys key [key ...] LEFT\|RIGHT [COUNT count]` | 7.0 | 从第一个非空列表批量弹出，返回 `[键, [元素 ...]]` |
| `BLMPOP timeout numkeys key [key ...] LEFT\|RIGHT [COUNT count]` | 7.0 | LMPOP 的阻塞版本，支持小数秒超时 |
| `ZMPOP numkeys key [key ...] MIN\|MAX [COUNT count]` | 7.0 | 从第一个非空有序集合批量弹出，返回 `[键, [[成员, 分数], ...]]` |
| `BZMPOP timeout numkeys key [key ...] MIN\|MAX [COUNT count]` | 7.0 | ZMPOP 的阻塞版本 |
| `CLIENT NO-EVICT on\|off` | 7.0 | 记录“不参与客户端驱逐”的标志；本版没有客户端驱逐，见范围说明 |
| `CLIENT NO-TOUCH on\|off` | 7.2 | 该连接访问键时不更新 LRU/LFU；`TOUCH` 仍会更新 |
| `ACL SETUSER/DELUSER/LIST/WHOAMI`、`AUTH username password` | 6.0 | 用户、口令、命令分类与键通配符；见下方范围说明 |
| `XADD ... [NOMKSTREAM] [MAXLEN\|MINID [~\|=] threshold [LIMIT n]]` | 6.2 | 追加时按长度或最小 ID 裁剪，支持近似裁剪与工作量上限；`NOMKSTREAM` 不自动建键 |
| `XTRIM key MAXLEN\|MINID [~\|=] threshold [LIMIT n]` | 6.2 | 按长度或最小 ID 裁剪；`LIMIT` 只能与 `~` 一起使用 |
| `XGROUP CREATECONSUMER key group consumer` | 6.2 | 显式创建消费者；`XREADGROUP ... NOACK` 创建的消费者也会同步到 AOF 和副本 |
| `XSETID key id [ENTRIESADDED n] [MAXDELETEDID id]` | 6.2 / 7.0 | 校正流计数；AOF 重写会保留这些计数 |
| `XPENDING key group [IDLE ms] start end count [consumer]` | 6.2 | 只返回空闲时间达到阈值的待处理消息 |
| `XAUTOCLAIM key group consumer min-idle start [COUNT n] [JUSTID]` | 6.2 | 批量转移待处理消息，返回游标，并清理流条目已删除的 PEL 记录 |
| `XINFO STREAM key [FULL [COUNT n]]` | 6.2 / 7.0 | `FULL` 输出条目、组、消费者与 PEL；摘要新增 `max-deleted-entry-id`、`entries-added`、`recorded-first-entry-id` |
| `BITCOUNT key [start end [BYTE\|BIT]]` | 7.0 | 按字节或按位范围统计置位数量 |
| `BITPOS key bit [start [end [BYTE\|BIT]]]` | 7.0 | 按字节或按位范围查找第一个 0/1 位 |
| `BITFIELD_RO key GET type offset ...` | 6.2 | 只读 BITFIELD，只允许 GET（与 OVERFLOW） |
| `LCS key1 key2 [LEN] [IDX] [MINMATCHLEN n] [WITHMATCHLEN]` | 7.0 | 最长公共子序列，可只取长度或输出匹配区间 |
| `SORT_RO key [BY ...] [LIMIT ...] [GET ...] [ASC\|DESC] [ALPHA]` | 7.0 | 只读 SORT；带 STORE 报错 |
| `EVAL_RO script numkeys key ...` / `EVALSHA_RO sha1 numkeys key ...` | 7.0 | 只读脚本入口，脚本里调用写命令直接报错 |
| `CLIENT PAUSE timeout [WRITE\|ALL]`、`CLIENT UNPAUSE`、`CLIENT INFO` | 6.2 | 暂停写命令或全部命令、提前恢复、返回当前连接信息 |
| `CONFIG GET pattern [pattern ...]`、`CONFIG SET param value [param value ...]` | 7.0 | 多模式查询（结果去重）与多参数设置；SET 按顺序应用，不是原子操作 |
| `COMMAND LIST [FILTERBY PATTERN\|ACLCAT\|MODULE ...]`、`COMMAND GETKEYSANDFLAGS` | 7.0 | 列出命令名；键标志按命令级读写属性近似给出 |
| `GEOSEARCH key FROMMEMBER\|FROMLONLAT ... BYRADIUS\|BYBOX ... [ASC\|DESC] [COUNT n [ANY]] [WITHCOORD\|WITHDIST\|WITHHASH]` | 6.2 | 按圆形或矩形区域搜索地理位置，支持 ANY 提前结束 |
| `GEOSEARCHSTORE dst src ... [STOREDIST]` | 6.2 | 把搜索结果写入有序集合，可存距离；源键不存在时删除目标键 |
| `ACL CAT [category]` / `ACL USERS` / `ACL GETUSER user` / `ACL GENPASS [bits]` / `ACL DRYRUN user cmd ...` | 6.0 | 列出命令分类与用户，查看用户规则，生成随机口令，预检权限 |
| `CLIENT KILL` 的 `LADDR` / `USER` / `MAXAGE` | 6.0 / 7.0 | 按服务端本地地址、认证用户、连接存活秒数筛选要断开的连接 |
| `CLIENT LIST` / `CLIENT INFO` 的 `laddr=`、`user=` 字段 | 6.0 / 6.2 | 输出连接的本地地址与当前认证用户 |
| `INFO everything` | 7.0 | 与 `INFO all` 相同地输出全部区段 |
| `WAITAOF numlocal numreplicas timeout` | 7.2 | 同步确认本机 AOF 已落盘；副本 AOF 计数固定为 0，见下方说明 |
| `SHUTDOWN [NOSAVE\|SAVE] [NOW] [FORCE] [ABORT]` | 7.0 | `NOW` 不等副本，`FORCE` 保存失败也退出，`ABORT` 取消待执行的关闭 |
| `ACL LOG [count\|RESET]` | 6.0 | 记录被拒绝的命令、键和认证失败，相同条目在 60 秒内合并计数；`acllog-max-len` 控制上限 |
| `ACL SAVE` / `ACL LOAD` 与 `aclfile` 配置 | 6.0 | 把用户规则写入文件并在启动或手动加载时恢复；规则文件损坏时保留内存中的旧用户 |
| `CLIENT SETINFO lib-name\|lib-ver` | 7.2 | 记录客户端库名称与版本，显示在 `CLIENT LIST`/`CLIENT INFO` 的 `lib-name=`、`lib-ver=` |
| `XGROUP SETID ... [ENTRIESREAD n]`、`XINFO GROUPS` 的 `entries-read`/`lag` | 7.0 | 记录消费组已读条目数并据此计算积压 |
| `INFO errorstats` | 6.2 | 按错误码统计回复数量，`CONFIG RESETSTAT` 清零 |
| `SSUBSCRIBE`/`SUNSUBSCRIBE`/`SPUBLISH`、`PUBSUB SHARDCHANNELS`/`SHARDNUMSUB` | 7.0 | 分片频道订阅与发布；单机下按本节点投递，与普通频道互不影响 |
| `LATENCY HISTOGRAM [command ...]` | 7.0 | 每条命令的调用次数与延迟直方图（1024ns 起、按 2 的幂分桶，回复单位为微秒） |
| `INFO latencystats` | 7.0 | 每条命令的 p50/p99/p99.9 延迟；`CONFIG RESETSTAT` 与 `LATENCY RESET` 清零 |
| `latency-tracking yes\|no` | 7.0 | 是否采集命令延迟直方图，默认 `yes` |
| ACL 频道权限：`&pattern`、`allchannels`、`resetchannels` | 6.0 | 对 `SUBSCRIBE`/`PSUBSCRIBE`/`PUBLISH`/`SSUBSCRIBE`/`SPUBLISH` 生效 |
| 脚本内的 ACL 检查 | 6.0 | `EVAL`/`EVALSHA` 里的 `redis.call` 按调用者的用户权限校验命令、键与频道 |
| `SCRIPT FLUSH [ASYNC\|SYNC]` | 6.2 | 两个选项都接受；本版在两种情况下都同步清空脚本缓存 |
| `COMMAND DOCS [command ...]` | 7.0 | 返回命令文档（summary/since/group/arguments）；无帮助条目的命令仍给出 `group=unknown` |
| `ZRANK` / `ZREVRANK` 的 `WITHSCORE` | 7.2 | 排名同时返回分数；成员或键不存在时返回空数组 |
| `ACL HELP` | 6.0 | 列出 ACL 子命令；`ACL WHOAMI` 继续对已认证用户放行 |
| `SCAN ... TYPE type` | 6.0 | 只返回指定类型的键；未知类型名返回空页 |
| `HELLO [protover [AUTH user pass] [SETNAME name]]` | 6.0 | 握手；本版只接受协议 2，拒绝 3；回复为 RESP2 扁平字段数组 |
| `CLIENT TRACKING` / `CACHING` / `GETREDIR` / `TRACKINGINFO` | 6.0 / 6.2 | 客户端缓存追踪；本版无 RESP3 push，失效经 Pub/Sub `__redis__:invalidate`（需 REDIRECT） |
| `tracking-table-max-keys` | 6.0 | TrackingTable 键数上限，CONFIG GET/SET/REWRITE |
| `INFO clients/stats`、`MEMORY STATS` tracking 字段 | 6.0 / 6.2 | `tracking_clients`、`tracking_total_*`、`clients.tracking.table.*` |
| `QUIT` | 1.0 | 正式进入命令表，可被 COMMAND/ACL 看到 |
| `XINFO CONSUMERS` 的 `inactive` | 7.2 | 距上次成功投递的毫秒数；尚未成功投递时为 -1；`idle` 仍按上次尝试交互计算 |

过期时间在秒数乘 1000、相对时间加当前时间之前做有符号 64 位溢出检查，非法值报错且不改动数据。

ACL 只做到 Redis 6 的范围：一个用户一套权限，支持 `on/off`、`>password`、`+@category/-@category`、`+cmd/-cmd`、`~keyglob`，并可用 `ACL CAT/USERS/GETUSER/GENPASS/DRYRUN` 查看规则和预检权限，用 `ACL LOG` 查看拒绝记录，用 `ACL SAVE`/`ACL LOAD` 配合 `aclfile` 持久化规则。没有 Redis 7 的多选择器（一个用户多套权限）和按键的读写权限位。频道规则已生效：`&pattern` 按通配符匹配字面频道，`PSUBSCRIBE` 要求与 ACL 模式字面一致，`allchannels` 放行全部频道，新建用户默认 `resetchannels`；`default` 用户默认拥有 `allchannels`。配置了 `requirepass` 时，它仍然是 `default` 用户的口令，未认证的连接一律拒绝；`CONFIG SET requirepass` 会同步更新 `default` 用户，不配置时连接直接以 `default` 用户身份进入。删除用户会断开仍以该用户认证的连接，不会留下悬空引用。脚本内调用的命令、键与频道同样按调用者的权限检查，越权时脚本报错而不是绕过 ACL。

命令语义参照 Redis `7.0.15`（BSD）手工适配，没有引入 Redis 8 的源码。完整说明见 [BACKPORT-RELEASE.md](BACKPORT-RELEASE.md)。

`CLIENT PAUSE WRITE` 只拦写命令与 `EXEC`，`CLIENT PAUSE ALL` 连读命令一起拦；被暂停的连接在恢复后按原顺序执行已解析的命令。`CONFIG SET` 多参数时逐对应用，后面的参数非法时前面的已经生效。`COMMAND LIST FILTERBY ACLCAT` 用命令级读写标志近似 `read`/`write`/`keyspace` 分类；`COMMAND GETKEYSANDFLAGS` 只给出 `RO`/`RW`、`access`、`update` 三种标志，没有 Redis 7 的键规格。`CLIENT KILL LADDR` 匹配的是服务端监听地址，因此同一监听地址上的所有连接都会命中；`MAXAGE` 按连接建立到现在的秒数计算。`LATENCY HISTOGRAM` 使用固定的对数桶，不是上游的 HDR 直方图，`INFO latencystats` 的百分位取桶上界，适合看数量级而不是精确尾延迟。`latency-tracking no` 只停止新采样，已有直方图保留到下一次重置。`WAITAOF` 在本版没有异步 fsync 确认机制：它在命令内同步刷盘后立即返回 `[本机确认, 0]`，不会阻塞到超时，也不跟踪副本的 AOF 落盘。`SHUTDOWN ABORT` 只能取消已置位但尚未执行的关闭请求，此时没有“关闭进行中”的状态。

## 没有合入的部分

以下能力不在本版中，调用会返回 `unknown command` 或沿用底座的旧行为：

- RESP3（`HELLO 3` 返回 NOPROTO；TRACKING 失效也不走 RESP3 push）、Redis Functions
- Redis 7 的 ACL 选择器与按键权限位（本版 ACL 停在 Redis 6 的单权限集）
- `COMMAND DOCS` 的完整参数树与 complexity/history 字段（本版用 `help.h` 的扁平字段近似）
- TLS、线程化 I/O
- 客户端内存驱逐（`maxmemory-clients`）。`CLIENT NO-EVICT` 只为兼容而接受并记录标志，本版不会因为内存压力驱逐客户端
- 多段 AOF（`appenddirname`）、新版 RDB 格式
- 集群、Redis 8 的查询引擎

持久化仍是底座的单文件 AOF 和原有 RDB。流的 RDB 格式没有升级，`entries-added` 与 `max-deleted-entry-id` 不写入 RDB 快照：重启后前者按当前长度恢复、后者归零；AOF（含重写）会保留这两个计数。带 TTL 的写入在 AOF 里记成普通 `SET` 加一条绝对时间的 `PEXPIREAT`，重放时不会把相对过期时间再算一遍。

## 构建

需要 Windows x64、Visual Studio 2022 的 C++ Build Tools（工具集 v143）和 Windows SDK。工程文件已从底座的 v142 改为 v143。

```powershell
.\build-tools\build-windows.ps1 -BuildTools 'D:\BuildTools\VS2022' -Toolset 'v143'
```

产物在 `msvs\x64\Release`：`redis-server.exe`、`redis-cli.exe`、`redis-benchmark.exe`。

只有 Windows x64 这一个构建。底座的工程文件、Win32 网络层和 jemalloc 都按 x64 编写，没有 Windows ARM64 目标，改平台配置编不出来。需要 ARM64 时只能在 Linux 上编译官方源码，不在本仓库范围内。

## 验证

在本机（Windows 11、MSVC 14.44 / v143）用当前源码编译后，72 项回归全部通过，测试使用临时目录和随机端口，不连接业务 Redis：

```bash
python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe
```

覆盖新命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟，以及 RDB 和 AOF 重启；本轮新增 tracking CONFIG/INFO/MEMORY、BCAST+PREFIX、QUIT/SCRIPT HELP/COMMAND INFO 校验，并保留 bp13 的 `CLIENT TRACKING`（Pub/Sub 失效）及更早用例。

这轮验证不等于已合入上游全部安全补丁，也没有做生产压力测试。

## 运行

启停、持久化、备份恢复和排查见 [OPS.md](OPS.md)。

## 许可证

本仓库初始提交带的是 Apache-2.0 的 `LICENSE`，那不是这份代码的许可证。代码本身沿用底座的 BSD 三条款许可证，见 [COPYING](COPYING)，各源文件头部保留原著作权声明。hiredis、jemalloc、lua 各自携带自己的许可证文件。
