# Redis for Windows 5.0.14.1-bp48

本版本以 tporadowski/redis 的 `v5.0.14.1` 标签为底座，选择性回移 Redis 6.0–7.2 的单机命令能力。它是自维护 Windows 移植版，不是 Redis 官方 6、7 或 8 发行版。

## 本次范围

| 能力 | 上游引入版本 | 本版行为与价值 |
| --- | --- | --- |
| `SET ... KEEPTTL` | 6.0 | 覆盖值时保留过期时间，避免客户端读 TTL 再写入的竞态 |
| `SET ... GET` | 6.2；支持 NX+GET 的语义取自 7.0 | 原子返回旧值并更新；遇到非字符串报错且不覆盖 |
| `SET ... EXAT/PXAT` | 6.2 | 接收绝对秒/毫秒过期时间 |
| `GETEX key [EX/PX/EXAT/PXAT time / PERSIST]` | 6.2 | 原子读取值并更新或去除 TTL；无选项时不更改 TTL |
| `GETDEL key` | 6.2 | 原子读取并删除字符串；并发争抢只能一个请求获得值 |
| `STRALGO LCS ...` | 6.0 | `LCS` 的旧命令别名，参数和回复与 `LCS` 一致 |
| 四种过期命令的 `NX/XX/GT/LT` | 7.0 | 条件更新 TTL；允许 XX+GT 或 XX+LT，拒绝冲突组合 |
| `EXPIRETIME/PEXPIRETIME` | 7.0 | 查询绝对过期时间；不存在返回 -2，无 TTL 返回 -1 |
| `SMISMEMBER key member ...` | 6.2 | 批量集合成员判断，减少请求次数，支持原有 intset/哈希表编码 |
| `ZMSCORE key member ...` | 6.2 | 批量有序集合分数查询，RESP2 返回字符串分数和空值 |
| `ZUNION` / `ZINTER` | 6.2 | 多键并集与交集，只返回不落库，支持 WEIGHTS 与 AGGREGATE |
| `ZDIFF` / `ZDIFFSTORE` | 6.2 | 差集；STORE 形式写入目标键 |
| `COPY` | 6.2 | 深拷贝字符串、列表、集合、有序集合、哈希和流，目标拥有独立存储与独立 TTL |
| `RESET` | 6.2 | 连接回到 0 号库并清除客户端状态 |
| `LPOS` | 6.0 | 查找列表元素下标，支持 RANK、COUNT、MAXLEN |
| `LMOVE` | 6.2 | 原子地在两个列表间移动元素，源和目标可相同 |
| `SINTERCARD` / `ZINTERCARD` | 7.0 | 只计算交集基数，支持 LIMIT 提前结束 |
| `ZADD ... GT\|LT` | 6.2 | 条件更新分数，新成员仍然直接加入 |
| `HRANDFIELD` / `ZRANDMEMBER` | 6.2 | 随机取样，支持正负 count 与 WITHVALUES/WITHSCORES |
| `LPOP` / `RPOP` 的 `count` | 6.2 | 批量弹出 |
| `BLMOVE` | 6.2 | 阻塞式原子移动 |
| 统一 `ZRANGE` / `ZRANGESTORE` | 6.2 | BYSCORE/BYLEX/REV/LIMIT，旧范围命令走同一实现 |
| `LMPOP` / `BLMPOP` | 7.0 | 从多个列表中选第一个非空键批量弹出，阻塞版本支持小数秒超时 |
| `ZMPOP` / `BZMPOP` | 7.0 | 从多个有序集合中选第一个非空键批量弹出，返回成员与分数对 |
| `CLIENT NO-EVICT` | 7.0 | 标记该连接不参与 `maxmemory-clients` 触发的客户端驱逐 |
| `CLIENT NO-TOUCH` | 7.2 | 该连接访问键时不更新 LRU/LFU，TOUCH 命令仍更新 |
| `lazyfree-lazy-user-del` / `lazyfree-lazy-user-flush` | 6.0 / 6.2 | 让 DEL 与 FLUSHDB/FLUSHALL 默认走异步释放，避免大键删除阻塞事件循环 |
| ACL（`SETUSER`/`DELUSER`/`LIST`/`WHOAMI`，`AUTH user pass`） | 6.0 | 用户、口令、命令分类与键通配符；见下方范围说明 |
| 过期时间边界检查 | 参考 7.0 实现 | 在秒乘 1000、相对时间加当前时间之前检查有符号 64 位溢出 |
| `XADD ... [NOMKSTREAM] [MAXLEN\|MINID [~\|=] threshold [LIMIT n]]` | 6.2 | 追加时按长度或最小 ID 裁剪；`NOMKSTREAM` 在键不存在时返回空而不建键 |
| `XTRIM key MAXLEN\|MINID [~\|=] threshold [LIMIT n]` | 6.2 | 按最小 ID 裁剪；`LIMIT` 限制单次删除的工作量，只能与 `~` 一起使用 |
| `XGROUP CREATECONSUMER` | 6.2 | 显式创建消费者；`XREADGROUP ... NOACK` 创建的消费者通过传播该命令同步到 AOF 和副本 |
| `XSETID key id [ENTRIESADDED n] [MAXDELETEDID id]` | 6.2 / 7.0 | 校正 `entries-added` 与 `max-deleted-entry-id`，并做单调性校验 |
| `XPENDING key group [IDLE ms] start end count [consumer]` | 6.2 | 按空闲时间过滤待处理消息 |
| `XAUTOCLAIM key group consumer min-idle start [COUNT n] [JUSTID]` | 6.2 | 批量转移待处理消息；返回下一游标；流条目已删除的 PEL 记录会被清理并在第三项返回 |
| `XINFO STREAM key [FULL [COUNT n]]` | 6.2 / 7.0 | `FULL` 输出条目、组、消费者及 PEL；摘要新增 `max-deleted-entry-id`、`entries-added`、`recorded-first-entry-id` |
| `BITCOUNT ... [BYTE\|BIT]` / `BITPOS ... [BYTE\|BIT]` | 7.0 | 范围单位可选字节或位；边界位用掩码处理，负下标按单位换算 |
| `BITFIELD_RO` | 6.2 | 只读 BITFIELD，只接受 GET；SET/INCRBY 报错且不修改数据 |
| `LCS` | 7.0 | 最长公共子序列；`LEN` 只返回长度，`IDX` 返回匹配区间，`MINMATCHLEN`/`WITHMATCHLEN` 控制区间输出 |
| `SORT_RO` | 7.0 | 只读 SORT；出现 `STORE` 直接报错 |
| `EVAL_RO` / `EVALSHA_RO` | 7.0 | 只读脚本入口；脚本执行期间调用任何写命令都会被拒绝 |
| `CLIENT PAUSE [WRITE\|ALL]` / `CLIENT UNPAUSE` / `CLIENT INFO` | 6.2 | 移植了 6.2 的 pending-command 机制：命令先解析再阻塞，恢复后按原顺序执行 |
| `CONFIG GET` 多模式 / `CONFIG SET` 多参数 | 7.0 | GET 对重叠模式去重；SET 逐对顺序应用，后一对失败时前面的已生效，不是原子操作 |
| `COMMAND LIST` / `COMMAND GETKEYSANDFLAGS` | 7.0 | 支持 FILTERBY PATTERN/ACLCAT/MODULE；ACLCAT 用命令级读写标志近似，MODULE 恒为空。键标志按每条命令的键规格给出，同一命令的不同键可以不同 |
| `GEOSEARCH` / `GEOSEARCHSTORE` | 6.2 | 圆形或矩形区域搜索；支持 FROMMEMBER/FROMLONLAT、BYRADIUS/BYBOX、ANY、WITHDIST/WITHHASH/WITHCOORD；STORE 形式可存距离，源键缺失时删除目标键 |
| `ACL CAT` / `ACL USERS` / `ACL GETUSER` / `ACL GENPASS` / `ACL DRYRUN` | 6.0 | 列出分类与用户、输出用户规则、生成随机口令、预检命令与键权限 |
| `CLIENT KILL` 的 `LADDR` / `USER` / `MAXAGE` | 6.0 / 7.0 | 按本地监听地址、认证用户和连接存活秒数筛选；LADDR 会命中同一监听地址上的全部连接 |
| `CLIENT LIST` / `CLIENT INFO` 的 `laddr=`、`user=` | 6.0 / 6.2 | 输出本地地址与当前认证用户名 |
| `INFO everything` | 7.0 | 与 `INFO all` 等价地输出全部区段 |
| `WAITAOF` | 7.2 | 同步刷盘并返回 `[本机确认, 0]`；不阻塞等待，不跟踪副本 AOF |
| `SHUTDOWN` 的 `NOW` / `FORCE` / `ABORT` | 7.0 | NOW 不等副本，FORCE 在保存失败时仍退出，ABORT 取消已置位的关闭请求 |
| `ACL LOG [count\|RESET]` | 6.0 | 记录命令、键与认证拒绝；相同条目在 60 秒内合并计数，`acllog-max-len` 限制长度 |
| `ACL SAVE` / `ACL LOAD` / `aclfile` | 6.0 | 用户规则写入文件并在启动或手动加载时恢复；加载失败保留旧用户，缺失用户会断开其连接 |
| `CLIENT SETINFO lib-name\|lib-ver` | 7.2 | 客户端库属性，长度上限 128，显示在 `CLIENT LIST`/`CLIENT INFO` |
| `XGROUP SETID ... ENTRIESREAD` / `XINFO GROUPS` 的 `entries-read`、`lag` | 7.0 | 记录消费组已读条目数并计算积压；阻塞读取也会累加 |
| `INFO errorstats` | 6.2 | 按错误码统计回复，包含共享错误对象；`CONFIG RESETSTAT` 清零 |
| 分片 Pub/Sub | 7.0 | `SSUBSCRIBE`/`SUNSUBSCRIBE`/`SPUBLISH` 与 `PUBSUB SHARDCHANNELS`/`SHARDNUMSUB`；单机按本节点投递 |
| `LATENCY HISTOGRAM [command ...]` | 7.0 | 每条命令的调用次数与延迟直方图；1024ns 起按 2 的幂分桶，回复单位为微秒 |
| `INFO latencystats` | 7.0 | 每条命令的 p50/p99/p99.9 延迟；`CONFIG RESETSTAT` 与 `LATENCY RESET` 清零 |
| `latency-tracking yes\|no` | 7.0 | 控制是否采集命令延迟直方图，默认开启 |
| ACL 频道权限（`&pattern`/`allchannels`/`resetchannels`） | 6.0 | 对订阅、模式订阅、发布与分片频道生效；模式订阅只接受字面匹配 |
| 脚本内 ACL 检查 | 6.0 | `EVAL`/`EVALSHA` 的 `redis.call` 按调用者用户校验命令、键与频道 |
| `SCRIPT FLUSH [ASYNC\|SYNC]` | 6.2 | `ASYNC` 把脚本缓存交给后台线程释放，`SYNC` 立即清空；Lua 解释器都在主线程重建 |
| `COMMAND DOCS [command ...]` | 7.0 | 返回命令文档（summary/complexity/since/group/arguments/history）；参数按层级展开，数据取自 Redis 7.0.15 的命令表 |
| `ZRANK` / `ZREVRANK` 的 `WITHSCORE` | 7.2 | 在排名后附带该成员分数；成员或键缺失时返回空数组 |
| `ACL HELP` | 6.0 | 列出 ACL 子命令；与 `ACL WHOAMI` 一样对已认证用户放行 |
| `SCAN ... TYPE type` | 6.0 | 键空间扫描可按类型过滤；未知类型名返回空页，不报错 |
| `HELLO [protover [AUTH user pass] [SETNAME name]]` | 6.0 | 握手；接受协议 2 和 3，其它版本返回 NOPROTO；协议 3 下回复为 RESP3 map |
| RESP3 push 帧 | 6.0 | 协议 3 下 Pub/Sub 的订阅确认、消息和 `CLIENT TRACKING` 失效以 `>` 推送，可与命令回复区分 |
| ACL 键读写权限（`%R~` / `%W~` / `%RW~`） | 7.0 | 每个键模式只授读、只授写或两者；按命令键规格区分读写，`SET`/`SETEX` 按命令级写标志近似 |
| `FUNCTION LOAD/DELETE/FLUSH/LIST/DUMP/RESTORE`、`FCALL`、`FCALL_RO` | 7.0 | Lua 函数按库管理并随 RDB/AOF 保存；DUMP 为本版私有格式 |
| `CLUSTER SHARDS` / `CLUSTER LINKS` | 7.0 | 分片视角的槽位与节点汇总；总线连接的方向、建立时间与收发计数 |
| `FAILOVER [TO host port [FORCE]] [ABORT] [TIMEOUT ms]` | 6.2 | 主库暂停普通客户端写入，等目标副本 ACK 追平后以副本身份 `PSYNC ... FAILOVER` 退位；目标副本自我提升 |
| `sanitize-dump-payload no\|yes\|clients` | 7.0 | `RESTORE` 前深校验 DUMP 内部编码：ziplist、listpack、intset、zipmap、quicklist、stream 的 PEL 与消费组；校验失败不落库 |
| `repl-diskless-load disabled\|on-empty-db\|swapdb` | 7.0 | 从库直接解析流式 RDB；`swapdb` 装进临时库，成功后一次性替换，失败保留旧数据 |
| `shutdown-timeout` | 7.0 | `SHUTDOWN` 等待副本 ACK 的毫秒上限，超时继续退出 |
| TLS 集群总线（`tls-cluster yes`） | 6.0 | 节点间总线握手、PING/PONG、槽位广播和故障转移投票走 TLS；只接受使用同一 CA 签发的节点证书 |
| `cluster-announce-hostname` / `cluster-preferred-endpoint` / `replica-announced` | 7.0 | 公告主机名、选择 `CLUSTER SLOTS`/`SHARDS` 按 IP 还是主机名给出地址、控制副本是否对外公告 |
| TLS 客户端监听（`tls-port`、PEM 证书、`tls-auth-clients` no/yes/optional） | 6.0 | OpenSSL 3 memory BIO 接入 IOCP；TLS 1.2/1.3；明文 `port` 可并存；`optional` 校验出示的客户端证书但允许匿名连接 |
| TLS 主从复制（`tls-replication`） | 6.0 | 从库连主库的 `tls-port`；握手、`PSYNC`、RDB 传输、增量命令与心跳全部加密；`repl-diskless-sync yes` 时经回环套接字由主进程转发 RDB |
| TLS 配置项（`tls-protocols`、`tls-ciphers`、`tls-ciphersuites`、`tls-prefer-server-ciphers`、`tls-session-caching`） | 6.0 | 协议版本、TLS 1.2 及以下的套件、TLS 1.3 套件、服务端套件优先和会话缓存；可 `CONFIG SET` 热应用 |
| RESP3 订阅态命令 | 7.0 | 协议 3 的订阅连接可继续执行普通命令；协议 2 保持订阅态限制 |
| AOF 时间戳注释（`aof-timestamp-enabled`） | 7.0 | 写入时按秒打 `#TS:<unix>` 注释，重写开头记录一次，加载跳过 |
| `CLIENT TRACKING` / `CACHING` / `GETREDIR` / `TRACKINGINFO` | 6.0 / 6.2 | 客户端缓存追踪；RESP3 直接 push 失效，RESP2 经 Pub/Sub `__redis__:invalidate`（需 REDIRECT） |
| `tracking-table-max-keys` CONFIG | 6.0 | TrackingTable 上限（0=不限，默认 1000000）；支持 GET/SET/REWRITE |
| `INFO`/`MEMORY STATS` tracking 字段 | 6.0 / 6.2 | `tracking_clients`、`tracking_total_*` 与 `clients.tracking.table.*` |
| `QUIT` 正式注册 | 1.0 | 进入命令表，可被 `COMMAND INFO`/`COUNT` 看到；Pub/Sub 与未认证场景仍可退出 |
| `XINFO CONSUMERS` 的 `inactive` | 7.2 | 距上次成功投递的毫秒；尚未成功投递为 -1；`idle` 仍按上次尝试交互 |
| `UNLINK` / `SWAPDB` / `WAIT` / `ROLE` 回归 | 4.0 / 4.0 / 3.0 / 2.8 | 异步删除、库交换、副本确认等待与角色查询；实现早已存在，bp16 补黑盒覆盖 |

ACL 支持 `on/off`、`>password`（SHA1 摘要保存）、`+@category/-@category`、`+cmd/-cmd`、`~keyglob`，并提供 `ACL CAT/USERS/GETUSER/GENPASS/DRYRUN`、`ACL LOG` 与 `ACL SAVE`/`ACL LOAD`。一个用户可以有多套权限：根规则之外用 `(...)` 追加选择器，根规则或任一选择器放行即允许该命令。键模式可以只授读（`%R~pattern`）、只授写（`%W~pattern`）或两者（`~pattern`、`%RW~pattern`）；命令对该键的访问方式取自键规格，`SET`/`SETEX` 这类随选项变化的按命令级写标志判断。频道规则已经生效：`&pattern` 按通配符匹配字面频道，`PSUBSCRIBE` 要求与 ACL 模式字面一致，`allchannels` 放行全部频道；新建用户默认 `resetchannels`，`default` 用户默认 `allchannels`。`requirepass` 成为 `default` 用户的口令，设置后未认证连接被拒绝，`CONFIG SET requirepass` 会同步更新该用户；不设置时连接以 `default` 用户进入。`ACL WHOAMI` 对任何已认证用户放行。删除用户或从 ACL 文件移除用户时，仍以该用户认证的连接会被断开，避免悬空指针。脚本里调用的命令、键与频道同样受调用者权限约束。

代码参照 Redis `7.0.15` 与 `7.2.5`（BSD）中的相关命令语义，按旧版 API 手工适配；没有引入 Redis 8 的源码或模块。

## 持久化适配

- 保留原有 RDB 格式和 Windows 后台保存机制；AOF 升级为 Redis 7 的多段布局，数据放在 `appenddirname`（默认 `appendonlydir`）目录，由 `appendonly.aof.manifest` 记录一个 base 文件和一个增量文件。重写完成后先写临时 manifest、再改名原子切换，随后删除旧文件；启动时如果只有旧的单文件 `appendonly.aof`，会先加载它，再在后台重写迁移到新布局。`redis-check-aof` 可直接接受该目录：按 manifest 顺序检查每个文件，`--fix` 只截断最后一个增量文件的坏尾。
- 流的 RDB 仍使用旧格式，`entries-added` 与 `max-deleted-entry-id` 不落快照：RDB 恢复后前者等于当前长度、后者归零；AOF 重放和重写通过带 `ENTRIESADDED`/`MAXDELETEDID` 的 `XSETID` 保留这两个计数。
- 成功的带 TTL 的 SET 在 AOF 中记录为普通 SET + 绝对 PEXPIREAT；KEEPTTL 记录实际保留的截止时间。
- GETEX 的写入记录为 PEXPIREAT、PERSIST 或 DEL/UNLINK；GETDEL 记录为 DEL。
- 条件过期仅记录成功结果的绝对截止时间，恢复时不重新判断 NX/XX/GT/LT。
- RESP3 接入了类型、握手和 push：`HELLO 3` 把该连接切到协议 3，map、set、null、bool、double 和 verbatim 按协议编码，`RESET` 退回协议 2。Pub/Sub 的订阅确认、消息、分片消息以及 TRACKING 失效在协议 3 下都是 `>` push 帧；协议 2 保持普通数组，TRACKING 失效仍走 Pub/Sub。协议 3 的订阅连接可以继续执行普通命令，协议 2 的订阅态限制不变。不迁入新版 RDB 格式，AOF 使用多段布局。TLS 覆盖客户端监听、主从复制链路（`tls-replication yes`）和集群总线（`tls-cluster yes`）；证书只在启动时加载，`CONFIG SET` 只调整协议、套件与会话参数，不重新读取证书文件。网络收发由多个 IO 线程分担，命令解析与执行仍在主线程。ACL 键权限按命令键规格区分读与写。`COMMAND DOCS` 的文档取自 Redis 7.0.15 的命令表，`ZRANK`/`ZREVRANK` 的 `WITHSCORE` 取自 7.2.5。原底座已有功能保留，但本版没有对其做现代化升级。
- `maxmemory-clients` 限制普通客户端的查询缓冲、回复缓冲和参数向量之和，超出时断开占用最大的连接。`CLIENT NO-EVICT` 的连接以及副本、主连接和 MONITOR 不参与。占用按这三项估算，不是 Redis 7 的客户端内存桶。
- `WAITAOF` 没有 7.2 的异步 fsync 完成队列：开启 AOF 时在命令内调用 `flushAppendOnlyFile` 并 `fsync`，然后立即返回；`numreplicas` 只做参数校验，回复中的副本数恒为 0，不阻塞到超时。
- `XINFO CONSUMERS` 的 `inactive`/`active_time` 是运行时数据，不写入本版 RDB：快照恢复后 `inactive` 回到 -1，直到再次成功投递。
- 命令延迟直方图是运行时数据，不写入 RDB/AOF；使用固定的对数桶而不是上游的 HDR 直方图，`INFO latencystats` 的百分位取桶上界。
- `SCRIPT FLUSH ASYNC` 换上一份空的脚本缓存，把旧的交给后台的延迟释放线程。Lua 解释器本身仍在主线程关闭并重建，因为跨线程释放它不安全。
- 集群沿用底座的 Redis 5.0 实现，槽位、握手、`MOVED`/`ASK` 重定向和故障转移代码未改写。bp25 增加三节点黑盒覆盖，并记录集群总线端口为客户端端口 +10000。bp35 增加 7.0 的 `CLUSTER ADDSLOTSRANGE`/`CLUSTER DELSLOTSRANGE`：按区间分配或删除槽位，支持一条命令给多段，参数、越界、区间倒置、重复和槽位状态都在应用前整体校验，任何一段不合法则整条命令不生效。bp36 增加 `CLUSTER SHARDS` 与 `CLUSTER LINKS`；bp37 让集群总线本身使用 TLS（`tls-cluster yes`，节点证书由同一 CA 签发，握手与投票走加密链路）；bp38 增加 `cluster-announce-hostname`、`cluster-preferred-endpoint` 与 `replica-announced`，并让 `CLUSTER NODES`/`SLOTS`/`SHARDS` 按所选端点输出地址。
- TLS 使用 OpenSSL 3.5.9 的 memory BIO：密文仍由 `read()` 和 `write()` 走原有 IOCP 就绪通知，不在 socket 上直接挂 `SSL_set_fd`。支持 `tls-port`、`tls-cert-file`、`tls-key-file`、`tls-ca-cert-file`、`tls-ca-cert-dir`、`tls-auth-clients`、`tls-replication`、`tls-cluster`、`tls-protocols`、`tls-ciphers`、`tls-ciphersuites`、`tls-prefer-server-ciphers` 和 `tls-session-caching`。`tls-cluster yes` 把节点间总线也接入同一套 memory BIO 收发路径，握手阶段双方校验证书链。`tls-auth-clients` 支持 `no`、`yes` 和 `optional` 三态，`yes` 与 `optional` 都要求先配置 CA。TLS 配置项在 `CONFIG SET` 时重建 SSL 上下文，新连接使用新设置，旧连接继续运行。主库向 TLS 从库发送 `+FULLRESYNC`、`+CONTINUE`、RDB 内容与心跳换行时都通过加密会话，磁盘方式传 RDB 由 TLS 写路径带背压发送。启用 `repl-diskless-sync` 时，TLS 从库由父进程从回环套接字读取 RDB 子进程的输出再写入 TLS 会话，不需要落盘；同时修好了底座的 RDB 子进程因 `rdbSaveRioWithEOFMark()` 参数不匹配而崩溃、导致无盘复制整体不可用的问题。运行时需要随包的 `libssl-3-x64.dll` 与 `libcrypto-3-x64.dll`。
- 网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。bp43 补齐无盘复制子进程写出 RDB 的调用签名（底座两参数原型与现实现不匹配，会让每次无盘全量同步崩溃），为 cluster/TLS 套接字补上 IOCP 关联声明，并在每次无盘同步前释放上一轮为子进程保存的 `WSAPROTOCOL_INFO` 数组、复制失败时让 fork 失败而不是把未初始化数据交给子进程。

## 性能预期

本版主要减少多步操作的网络往返，并提供单命令原子性。SMISMEMBER/ZMSCORE 可将 N 条查询合为一条；收益受批量大小、网络延迟和客户端流水线方式影响。不能据此宣称整体吞吐与 Redis 6/7/8 相同，也没有更换内存分配器、数据编码或通讯层。

bp44 用 `build-tools/bench-hotpaths.py` 在同一进程内交替测量 bp28 与当前构建（回环、单客户端、32 条命令一条流水线，每项取最好一轮）：PING、SET/GET、GET 缺失、MGET×10、HSET/HGET、SADD/SMISMEMBER、ZADD/ZMSCORE、LPUSH/LRANGE、EVAL、INFO、CONFIG GET 等路径当前构建比 bp28 快 5%–12%，DBSIZE 持平，没有发现回移造成的性能回退。`sanitize-dump-payload yes` 的深校验按 payload 大小线性计费，200 项列表的 `RESTORE` 吞吐约下降 9%；默认值 `no` 不产生这项开销。

不能用版本号判断完整兼容性。没有合入的新版命令仍会返回 unknown command；跨版本的数据迁移应先备份，再验证所用数据类型与格式。

## 构建

需要 Windows x64、MSVC C++ Build Tools 和 Windows SDK。本机安装的是 VS2022 Build Tools（MSVC 14.44，工具集 v143），工程文件已从 v142 改为 v143。构建脚本默认工具位置为 `D:\BuildTools\VS2022`，可用参数覆盖。

```powershell
.\build-tools\build-windows.ps1 -BuildTools 'D:\BuildTools\VS2022' -Toolset 'v143'
```

产物位于 `msvs/x64/Release`。TLS 依赖 `deps/openssl` 下的 OpenSSL 3.5.9 头和 import lib；运行时从 `deps/openssl/bin` 取 `libssl-3-x64.dll`、`libcrypto-3-x64.dll`，打包脚本会把两个 DLL 和 `deps/openssl/LICENSE.txt` 放入发布包。编译脚本会生成 `src/release.h`，写入底座 Git SHA 和工作区 dirty 状态；不自动提交或创建 Git 标签。

## 验证

使用 Python 标准库的黑盒测试，启动独立临时实例，不连接现有业务 Redis：

```bash
python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe
```

覆盖新命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟及 RDB/AOF 重启。本轮覆盖 `FUNCTION` / `FCALL` 及 DUMP/RESTORE 往返、三节点集群组网与重定向，以及 TLS 明文/密文双监听、256 KiB 往返、流水线和 `tls-auth-clients yes` 客户端证书校验，RESP3 下 Pub/Sub 与 TRACKING 的 push 帧，ACL 的 `%R~`/`%W~` 键权限，TLS 主从复制的全量同步、部分重同步与增量传播，TLS 协议/套件/会话配置的运行时校验与热应用，`tls-auth-clients` 三态切换与不受信客户端证书的拒绝，无盘复制下 TLS 与明文从库的并行全量同步，RESP3 订阅连接执行普通命令，`new`/`keymiss` 通知事件，以及 AOF 时间戳注释的写入、重写与重载，`CLUSTER ADDSLOTSRANGE`/`DELSLOTSRANGE` 的区间分配、删除、错误校验与槽位释放后的 `CLUSTERDOWN`，`CLUSTER SHARDS`/`LINKS`，集群总线 TLS 组网与节点证书校验，`cluster-announce-hostname`/`cluster-preferred-endpoint`/`replica-announced`，`shutdown-timeout` 的等待与超时，`repl-diskless-load` 三种模式及失败回退，`sanitize-dump-payload` 三模式、CORRUPT payload 拒绝与 `CONFIG REWRITE`，`FAILOVER` 的 ABORT、超时恢复、`TO` 指定目标与自动选目标，以及 Windows 底座在持续写入下的 BGSAVE/BGREWRITEAOF、AOF 运行时开关、3 字节分段写入、1 MiB value、1500 条流水线、连接抖动和空闲超时；共 118 项。保留 bp23 的 RESP3 及 bp22 及更早用例。

这是一轮选择性回移，不等于已完成上游所有安全补丁或生产压力验证。生产投放仍需在业务负载下检验 Redisson 调用、后台持久化、异常断电恢复和长时间运行。

## 来源与许可证

- Windows 底座：https://github.com/tporadowski/redis/releases/tag/v5.0.14.1
- 命令参考：https://github.com/redis/redis/tree/7.0.15
- TLS：OpenSSL 3.5.9（Apache License 2.0），Windows x64 二进制来自 Win64OpenSSL 3.5.9 安装包，许可证随包放在 `licenses/openssl-LICENSE.txt`。
- 随包保留 `COPYING` 及原有源码版权声明。
