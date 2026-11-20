# Redis for Windows 5.0.14.1-bp75

Windows x64 上的单机 Redis，以 [tporadowski/redis](https://github.com/tporadowski/redis) 的
`v5.0.14.1`（提交 `ec77f72dc`，2022-02 发布）为底座，选择性回移了 Redis 6.0–7.2 的一批单机命令。

它不是 Redis 官方发行版，也不是 Redis 6、7 或 8。`redis-server --version` 报告的版本号是 `5.0.14.1-bp65`。

## 这个版本新增了什么

| 命令 | 上游引入版本 | 行为 |
| --- | --- | --- |
| `SET key value [NX\|XX] [GET] [EX\|PX\|EXAT\|PXAT time\|KEEPTTL]` | 6.0 / 6.2；NX+GET 取自 7.0 | 覆盖时可保留原 TTL，可原子返回旧值，可按绝对时间过期 |
| `GETEX key [EX\|PX\|EXAT\|PXAT time\|PERSIST]` | 6.2 | 原子读取并更新或去掉 TTL；不带选项时不改 TTL |
| `GETDEL key` | 6.2 | 原子读取并删除字符串 |
| `STRALGO LCS ...` | 6.0 | `LCS` 的旧命令别名，参数和回复与 `LCS` 一致 |
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
| `CLIENT NO-EVICT on\|off` | 7.0 | 标记该连接不参与 `maxmemory-clients` 触发的客户端驱逐 |
| `CLIENT NO-TOUCH on\|off` | 7.2 | 该连接访问键时不更新 LRU/LFU；`TOUCH` 仍会更新 |
| `ACL SETUSER/DELUSER/LIST/WHOAMI`、`AUTH username password` | 6.0 | 用户、口令、命令分类与键通配符；见下方范围说明 |
| `XADD ... [NOMKSTREAM] [MAXLEN\|MINID [~\|=] threshold [LIMIT n]]` | 6.2 | 追加时按长度或最小 ID 裁剪，支持近似裁剪与工作量上限；`NOMKSTREAM` 不自动建键 |
| `XTRIM key MAXLEN\|MINID [~\|=] threshold [LIMIT n]` | 6.2 | 按长度或最小 ID 裁剪；`LIMIT` 只能与 `~` 一起使用 |
| `XGROUP CREATECONSUMER key group consumer` | 6.2 | 显式创建消费者；`XREADGROUP ... NOACK` 创建的消费者也会同步到 AOF 和副本 |
| `XSETID key id [ENTRIESADDED n] [MAXDELETEDID id]` | 6.2 / 7.0 | 校正流计数；RDB 快照与 AOF 重写都会保留这些计数 |
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
| `WAITAOF numlocal numreplicas timeout` | 7.2 | 等待本机 AOF fsync 与副本 FACK；无副本时副本确认返回 0 |
| `SHUTDOWN [NOSAVE\|SAVE] [NOW] [FORCE] [ABORT]` | 7.0 | `NOW` 不等副本，`FORCE` 保存失败也退出，`ABORT` 取消待执行的关闭 |
| `ACL LOG [count\|RESET]` | 6.0 | 记录被拒绝的命令、键和认证失败，相同条目在 60 秒内合并计数；`acllog-max-len` 控制上限 |
| `ACL SAVE` / `ACL LOAD` 与 `aclfile` 配置 | 6.0 | 把用户规则写入文件并在启动或手动加载时恢复；规则文件损坏时保留内存中的旧用户 |
| `CLIENT SETINFO lib-name\|lib-ver` | 7.2 | 记录客户端库名称与版本，显示在 `CLIENT LIST`/`CLIENT INFO` 的 `lib-name=`、`lib-ver=` |
| `XGROUP SETID ... [ENTRIESREAD n]`、`XINFO GROUPS` 的 `entries-read`/`lag` | 7.0 | 记录消费组已读条目数并据此计算积压；快照重启后仍保留 |
| `INFO errorstats` | 6.2 | 按错误码统计回复数量，`CONFIG RESETSTAT` 清零 |
| `SSUBSCRIBE`/`SUNSUBSCRIBE`/`SPUBLISH`、`PUBSUB SHARDCHANNELS`/`SHARDNUMSUB` | 7.0 | 分片频道订阅与发布；单机下按本节点投递，与普通频道互不影响 |
| `LATENCY HISTOGRAM [command ...]` | 7.0 | 每条命令的调用次数与延迟直方图（1024ns 起、按 2 的幂分桶，回复单位为微秒） |
| `INFO latencystats` | 7.0 | 每条命令的 p50/p99/p99.9 延迟；`CONFIG RESETSTAT` 与 `LATENCY RESET` 清零 |
| `latency-tracking yes\|no` | 7.0 | 是否采集命令延迟直方图，默认 `yes` |
| ACL 频道权限：`&pattern`、`allchannels`、`resetchannels` | 6.0 | 对 `SUBSCRIBE`/`PSUBSCRIBE`/`PUBLISH`/`SSUBSCRIBE`/`SPUBLISH` 生效 |
| 脚本内的 ACL 检查 | 6.0 | `EVAL`/`EVALSHA` 里的 `redis.call` 按调用者的用户权限校验命令、键与频道 |
| `SCRIPT FLUSH [ASYNC\|SYNC]` | 6.2 | `ASYNC` 把脚本缓存交给后台线程释放，`SYNC` 立即清空；Lua 解释器都在主线程重建 |
| `COMMAND DOCS [command ...]` | 7.0 | 返回命令文档（summary/complexity/since/group/arguments/history）；参数按层级展开，没有文档的命令给出 `group=unknown` |
| `ZRANK` / `ZREVRANK` 的 `WITHSCORE` | 7.2 | 排名同时返回分数；成员或键不存在时返回空数组 |
| `ACL HELP` | 6.0 | 列出 ACL 子命令；`ACL WHOAMI` 继续对已认证用户放行 |
| `SCAN ... TYPE type` | 6.0 | 只返回指定类型的键；未知类型名返回空页 |
| `HELLO [protover [AUTH user pass] [SETNAME name]]` | 6.0 | 握手；接受协议 2 和 3，其它版本返回 NOPROTO；协议 3 下回复为 RESP3 map |
| RESP3 push 帧 | 6.0 | 协议 3 下 Pub/Sub 的订阅确认、消息和 `CLIENT TRACKING` 失效用 `>` 推送；RESP2 仍是普通数组，失效继续走 `__redis__:invalidate` 频道 |
| RESP3 订阅态命令 | 7.0 | 协议 3 的订阅连接可以继续执行普通命令，回包与 push 消息可区分；协议 2 仍只允许订阅管理和 PING/QUIT/RESET |
| keyspace 通知 `n` / `m` | 7.0 | 新增键发 `new` 事件，读取不存在的键发 `keymiss` 事件；`A` 别名不含这两类 |
| AOF 时间戳注释（`aof-timestamp-enabled`） | 7.0 | 每次写入最多每秒追加一条 `#TS:<unix>` 注释，重写时在文件开头记录一次；加载时跳过注释行 |
| 多段 AOF（`appenddirname`、manifest、base/增量文件） | 7.0 | AOF 位于 `appendonlydir`，manifest 记录 base 与增量文件；重写原子替换 manifest，旧文件在切换后删除；单文件旧布局启动后自动迁移 |
| ACL 键读写权限（`%R~` / `%W~` / `%RW~`） | 7.0 | 每个键模式可只授读、只授写或两者；按命令键规格区分读写，`SET` 等选项相关命令按命令级写标志判断 |
| `CLUSTER ADDSLOTSRANGE start end [start end ...]` / `CLUSTER DELSLOTSRANGE start end [start end ...]` | 7.0 | 按区间分配或删除本节点槽位，一条命令可以给多段；任一段非法则整条命令不生效 |
| `CLUSTER SHARDS [shard-id]` / `CLUSTER LINKS` | 7.0 | 按分片汇总槽位与主从节点；列出集群总线连接的方向、建立时间与收发字节 |
| `FAILOVER [TO host port [FORCE]] [ABORT] [TIMEOUT ms]` | 6.2 | 主库把手里的写请求暂停，等副本追平后主动退位；`TO` 指定目标副本，`ABORT` 取消，`TIMEOUT` 限时 |
| `sanitize-dump-payload no\|yes\|clients` | 7.0 | `RESTORE` 前深校验 DUMP 内部的 ziplist/listpack/intset/zipmap/stream 结构，而不只校验版本与 CRC |
| `repl-diskless-load disabled\|on-empty-db\|swapdb` | 7.0 | 从库把收到的无盘 RDB 直接解析；`swapdb` 先装进临时库，成功后整体替换 |
| `shutdown-timeout` | 7.0 | `SHUTDOWN` 等待副本确认的毫秒数，超时才继续退出 |
| TLS 集群总线（`tls-cluster yes`） | 6.0 | 节点之间的总线握手、PING/PONG、槽位广播与故障转移投票走 TLS；要求所有节点使用同一 CA |
| `cluster-announce-hostname` / `cluster-preferred-endpoint` / `replica-announced` | 7.0 | 公告节点主机名，选择客户端按 IP 还是主机名连接，以及副本是否出现在 `CLUSTER SLOTS`/`SHARDS` 中 |
| `FUNCTION LOAD/DELETE/FLUSH/LIST/DUMP/RESTORE`、`FCALL`、`FCALL_RO` | 7.0 | 按库管理 Lua 函数，函数源码随 RDB/AOF 保存；DUMP 为本版私有格式 |
| TLS 客户端监听（`tls-port`、证书、`tls-auth-clients` 三态客户端证书认证） | 6.0 | 用 OpenSSL 3 在现有 Windows IOCP 连接上做 TLS 1.2/1.3；明文 `port` 可同时保留 |
| TLS 主从复制（`tls-replication`） | 6.0 | 从库连主库的 `tls-port`，握手、PSYNC、RDB 传输和增量流都走加密连接；支持 `repl-diskless-sync` 无盘全量同步 |
| TLS 配置项（`tls-protocols`、`tls-ciphers`、`tls-ciphersuites`、`tls-prefer-server-ciphers`、`tls-session-caching`） | 6.0 | 限定协议版本与套件、服务端优先选择套件、开关会话缓存；可按配置启动，也可 `CONFIG SET` 热应用，新连接生效 |
| `CLIENT TRACKING` / `CACHING` / `GETREDIR` / `TRACKINGINFO` | 6.0 / 6.2 | 客户端缓存追踪；RESP3 连接直接收到 push 失效，RESP2 连接仍需 `REDIRECT` 到 `__redis__:invalidate` 订阅者 |
| `tracking-table-max-keys` | 6.0 | TrackingTable 键数上限，CONFIG GET/SET/REWRITE |
| `INFO clients/stats`、`MEMORY STATS` tracking 字段 | 6.0 / 6.2 | `tracking_clients`、`tracking_total_*`、`clients.tracking.table.*` |
| `QUIT` | 1.0 | 正式进入命令表，可被 COMMAND/ACL 看到 |
| `XINFO CONSUMERS` 的 `inactive` | 7.2 | 距上次成功投递的毫秒数；尚未成功投递时为 -1；`idle` 仍按上次尝试交互计算；`XCLAIM`/`XAUTOCLAIM`（含 `JUSTID`）刷新投递时钟 |
| `UNLINK` / `SWAPDB` / `WAIT` / `ROLE` 回归 | 4.0 / 4.0 / 3.0 / 2.8 | 异步删除、库交换、副本确认等待与角色查询；实现早已存在，bp16 补黑盒覆盖 |

过期时间在秒数乘 1000、相对时间加当前时间之前做有符号 64 位溢出检查，非法值报错且不改动数据。

ACL 支持 `on/off`、`>password`、`+@category/-@category`、`+cmd/-cmd`、`~keyglob`，并可用 `ACL CAT/USERS/GETUSER/GENPASS/DRYRUN` 查看规则和预检权限，用 `ACL LOG` 查看拒绝记录，用 `ACL SAVE`/`ACL LOAD` 配合 `aclfile` 持久化规则。一个用户可以有多套权限：根规则之外，用 `(...)` 追加选择器，根规则或任一选择器放行即允许该命令。键模式可以只授读（`%R~pattern`）、只授写（`%W~pattern`）或两者（`~pattern`、`%RW~pattern`）；`~pattern` 与 `~*`、`allkeys` 等价于读写都允许。命令是否被允许取决于它对该键的实际访问方式：只读命令要求读权限，写入、插入、删除要求写权限，`GETDEL`、`COPY` 的目标键等同时要求两者；`SET`、`SETEX` 的访问方式取决于选项，按命令级写标志判断。频道规则已生效：`&pattern` 按通配符匹配字面频道，`PSUBSCRIBE` 要求与 ACL 模式字面一致，`allchannels` 放行全部频道，新建用户默认 `resetchannels`；`default` 用户默认拥有 `allchannels`。配置了 `requirepass` 时，它仍然是 `default` 用户的口令，未认证的连接一律拒绝；`CONFIG SET requirepass` 会同步更新 `default` 用户，不配置时连接直接以 `default` 用户身份进入。删除用户会断开仍以该用户认证的连接，不会留下悬空引用。脚本内调用的命令、键与频道同样按调用者的权限检查，越权时脚本报错而不是绕过 ACL。

命令语义参照 Redis `7.0.15`（BSD）手工适配，没有引入 Redis 8 的源码。完整说明见 [BACKPORT-RELEASE.md](BACKPORT-RELEASE.md)。

集群模式沿用底座的 Redis 5.0 集群实现：`cluster-enabled yes` 启动后使用 16384 个槽，支持 `CLUSTER MEET`、`ADDSLOTS`、`SLOTS`、`NODES`、`KEYSLOT`、`COUNTKEYSINSLOT`、`REPLICATE` 等命令。bp25 用三个本机节点验证了组网、槽位分配、`MOVED` 重定向和槽位撤下后的 `CLUSTERDOWN`；bp35 增加 `CLUSTER ADDSLOTSRANGE`/`CLUSTER DELSLOTSRANGE`，按区间分配和删除槽位；bp36 增加 `CLUSTER SHARDS [shard-id]` 与 `CLUSTER LINKS`，bp37 让总线本身也能走 TLS（`tls-cluster yes`），bp38 加入 `cluster-announce-hostname`、`cluster-preferred-endpoint` 与 `replica-announced`；集群总线端口固定为客户端端口 +10000，规划端口时要同时预留。

TLS 监听通过 `tls-port` 开启，`tls-cert-file`/`tls-key-file` 指定 PEM 证书链和私钥；设置 `tls-ca-cert-file` 后可用 `tls-auth-clients yes` 要求客户端证书；`optional` 会校验客户端出示的证书，同时允许不带证书的匿名连接。明文 `port` 可以同时保留，方便本地运维连接。TLS 由 OpenSSL 3 的 memory BIO 接入现有 IOCP 收发路径，不改变命令解析与执行线程模型；程序目录必须同时有 `libssl-3-x64.dll` 和 `libcrypto-3-x64.dll`。随包 `redis-cli.exe` 只支持明文；传入 `--tls` 等 TLS 参数会给出明确错误提示，连接加密端口需使用支持 TLS 的客户端。从库设置 `tls-replication yes` 后可以连主库的 `tls-port`，握手、`PSYNC`、RDB 传输和后续增量命令都在同一个 TLS 会话里；主库给从库回 `+FULLRESYNC`、`+CONTINUE`、RDB 内容和心跳换行时同样走加密通道，主库启用 `repl-diskless-sync yes` 时，TLS 从库也能无盘同步：RDB 子进程把数据写进一对回环套接字，由主进程转发进 TLS 会话，不再落盘。 `tls-protocols` 用空格分隔 `TLSv1`/`TLSv1.1`/`TLSv1.2`/`TLSv1.3`；`tls-ciphers` 管 TLS 1.2 及以下，`tls-ciphersuites` 管 TLS 1.3；`tls-session-caching no` 关闭会话复用。这几项与 `tls-prefer-server-ciphers`、`tls-auth-clients` 都支持 `CONFIG SET`，重建上下文后对新建连接生效，已有连接不受影响。

`CLIENT PAUSE WRITE` 只拦写命令与 `EXEC`，`CLIENT PAUSE ALL` 连读命令一起拦；被暂停的连接在恢复后按原顺序执行已解析的命令。`CONFIG SET` 多参数时逐对应用，后面的参数非法时前面的已经生效。`COMMAND LIST FILTERBY ACLCAT` 用命令级读写标志近似 `read`/`write`/`keyspace` 分类；`COMMAND GETKEYSANDFLAGS` 按每条命令的键规格给出标志，同一条命令的不同键可以不同，例如 `BITOP` 的目标键是 `OW update`、源键是 `RO access`。`SET ... GET` 的键标志按当前参数解析为读写；键位置仍不固定的参数（`SORT` 的 `STORE`）按命令级标志近似。`CLIENT KILL LADDR` 匹配的是服务端监听地址，因此同一监听地址上的所有连接都会命中；`MAXAGE` 按连接建立到现在的秒数计算。`LATENCY HISTOGRAM` 使用每八度四等分的固定对数桶（256 ns 起），不是上游的 HDR 直方图，`INFO latencystats` 的百分位取桶上界，比整倍桶更细但仍是估算值。`latency-tracking no` 只停止新采样，已有直方图保留到下一次重置。`WAITAOF` 会等待覆盖此前写入的本机 AOF fsync：`appendfsync always/no` 下命令内同步 fsync；`appendfsync everysec` 下把 fsync 放入后台线程并阻塞客户端直到该次 fsync 完成或超时，超时返回当前已获得的确认数。副本通过 `REPLCONF ACK ... FACK` 回传已 fsync 的复制偏移，主库据此统计副本确认；无副本时请求的副本数会归零并立即返回。`SHUTDOWN ABORT` 只能取消已置位但尚未执行的关闭请求，此时没有“关闭进行中”的状态。

## 没有合入的部分

以下能力不在本版中，调用会返回 `unknown command` 或沿用底座的旧行为：

- `tls-cluster` 的开关已支持 `CONFIG SET` 热切换并重建集群总线 TLS 上下文；切换为 `yes` 时会校验证书、私钥和 CA 配置，失败自动回滚；普通 TLS 监听的证书、私钥和 CA 文件同样支持 `CONFIG SET` 热加载，对新建连接生效，已有连接不受影响
- 客户端内存驱逐按查询缓冲、参数向量、回复缓冲及额外内存桶估算；`CLIENT INFO`/`LIST` 只公开其中主要桶，不按 Redis 7 的全部内存桶逐项公开
- 新版 RDB 整体格式（当前 `RDB_VERSION 11` 仅加入流计数与消费者活跃时间兼容扩展，不等同于 Redis 7/8 RDB）
- Redis 8 的查询引擎

持久化使用多段 AOF（`appendonlydir`、manifest、base 与增量文件）和原有的 RDB 格式。`redis-check-aof` 既能检查单个 AOF 文件，也能接受 `appendonlydir` 目录：按 manifest 顺序检查每个 base/增量文件，`--fix` 只截断最后一个增量文件的坏尾。仍然可以对具体的 `.incr.aof`/`.base.aof` 单独执行。`appenddirname` 与 `appendfilename` 一样只能在启动配置里设置，`CONFIG GET` 可读、`CONFIG REWRITE` 会写回，但不支持 `CONFIG SET`。流的快照在 Redis 5 的 RDB 版本内用兼容扩展保存 `entries-added`、`max-deleted-entry-id` 和消费组 `entries-read`；旧版快照仍按当前长度恢复计数。AOF（含重写）继续通过 `XSETID`/`XGROUP SETID` 保留这些计数。带 TTL 的写入在 AOF 里记成普通 `SET` 加一条绝对时间的 `PEXPIREAT`，重放时不会把相对过期时间再算一遍。

## 构建

需要 Windows x64、Visual Studio 2022 的 C++ Build Tools（工具集 v143）和 Windows SDK。工程文件已从底座的 v142 改为 v143。TLS 依赖仓库内 `deps\openssl` 的 OpenSSL 3.5.9 头文件与 import lib，运行时还需要同目录的 `libssl-3-x64.dll` 和 `libcrypto-3-x64.dll`。

```powershell
.\build-tools\build-windows.ps1 -BuildTools 'D:\BuildTools\VS2022' -Toolset 'v143'
```

产物在 `msvs\x64\Release`：`redis-server.exe`、`redis-cli.exe`、`redis-benchmark.exe`。

只有 Windows x64 这一个构建。底座的工程文件、Win32 网络层和 jemalloc 都按 x64 编写，没有 Windows ARM64 目标，改平台配置编不出来。需要 ARM64 时只能在 Linux 上编译官方源码，不在本仓库范围内。

## 验证

在本机（Windows 11、MSVC 14.44 / v143）用当前源码编译后，138 项回归全部通过，测试使用临时目录和随机端口，不连接业务 Redis：

```bash
python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe
```

覆盖新命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟，以及 RDB 和 AOF 重启；本轮覆盖 `FUNCTION` / `FCALL` 及 DUMP/RESTORE 往返、三节点集群组网与重定向、TLS 明文/密文双监听、256 KiB 往返、流水线和客户端证书认证，TLS 主从复制的全量同步、部分重同步与增量传播，TLS 协议/套件/会话配置的运行时校验与热应用，`tls-auth-clients` 在 `no`/`yes`/`optional` 之间切换及不受信客户端证书的拒绝，无盘复制下 TLS 与明文从库的并行全量同步，RESP3 订阅连接执行普通命令，`new`/`keymiss` 通知事件，以及 AOF 时间戳注释的写入、重写与重载，RESP3 下 Pub/Sub 与 `CLIENT TRACKING` 的 push 帧（RESP2 仍使用数组和 Pub/Sub 频道），以及 ACL 的 `%R~`/`%W~` 键读写权限，`CLUSTER ADDSLOTSRANGE`/`DELSLOTSRANGE` 的区间分配、删除与错误校验，`CLUSTER SHARDS [shard-id]`/`LINKS` 的输出，集群总线 TLS 组网，`cluster-announce-hostname` 与 `replica-announced`，`shutdown-timeout` 等待副本，`repl-diskless-load` 的 `on-empty-db`/`swapdb`，`sanitize-dump-payload` 的三种模式与损坏 payload 拒绝，`FAILOVER` 的 ABORT/超时/指定目标/自动选目标，以及 Windows 底座在持续写入下的 BGSAVE/BGREWRITEAOF、AOF 运行时开关、分段写入、大 value、连接抖动和空闲超时；保留 bp23 的 RESP3 和 bp22 及更早用例。

这轮验证不等于已合入上游全部安全补丁，也没有做生产压力测试。

## 运行

启停、持久化、备份恢复和排查见 [OPS.md](OPS.md)。

## 许可证

本仓库初始提交带的是 Apache-2.0 的 `LICENSE`，那不是这份代码的许可证。代码本身沿用底座的 BSD 三条款许可证，见 [COPYING](COPYING)，各源文件头部保留原著作权声明。hiredis、jemalloc、lua 各自携带自己的许可证文件。
