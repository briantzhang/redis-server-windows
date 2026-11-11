# Redis for Windows 5.0.14.1-bp16

本版本以 tporadowski/redis 的 `v5.0.14.1` 标签为底座，选择性回移 Redis 6.0–7.2 的单机命令能力。它是自维护 Windows 移植版，不是 Redis 官方 6、7 或 8 发行版。

## 本次范围

| 能力 | 上游引入版本 | 本版行为与价值 |
| --- | --- | --- |
| `SET ... KEEPTTL` | 6.0 | 覆盖值时保留过期时间，避免客户端读 TTL 再写入的竞态 |
| `SET ... GET` | 6.2；支持 NX+GET 的语义取自 7.0 | 原子返回旧值并更新；遇到非字符串报错且不覆盖 |
| `SET ... EXAT/PXAT` | 6.2 | 接收绝对秒/毫秒过期时间 |
| `GETEX key [EX/PX/EXAT/PXAT time / PERSIST]` | 6.2 | 原子读取值并更新或去除 TTL；无选项时不更改 TTL |
| `GETDEL key` | 6.2 | 原子读取并删除字符串；并发争抢只能一个请求获得值 |
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
| `CLIENT NO-EVICT` | 7.0 | 接受并记录标志；本版没有客户端内存驱逐，因此不会产生实际驱逐效果 |
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
| `COMMAND LIST` / `COMMAND GETKEYSANDFLAGS` | 7.0 | 支持 FILTERBY PATTERN/ACLCAT/MODULE；ACLCAT 用命令级读写标志近似，MODULE 恒为空。键标志只给出 RO/RW、access、update |
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
| `SCRIPT FLUSH [ASYNC\|SYNC]` | 6.2 | 接受两个选项；本版在两种情况下都同步清空脚本缓存 |
| `COMMAND DOCS [command ...]` | 7.0 | 返回命令文档字段（summary/since/group/arguments）；数据来自 `help.h`，未知命令名被跳过 |
| `ZRANK` / `ZREVRANK` 的 `WITHSCORE` | 7.2 | 在排名后附带该成员分数；成员或键缺失时返回空数组 |
| `ACL HELP` | 6.0 | 列出 ACL 子命令；与 `ACL WHOAMI` 一样对已认证用户放行 |
| `SCAN ... TYPE type` | 6.0 | 键空间扫描可按类型过滤；未知类型名返回空页，不报错 |
| `HELLO [protover [AUTH user pass] [SETNAME name]]` | 6.0 | 握手；只接受协议 2，拒绝 3；回复为 RESP2 扁平字段数组 |
| `CLIENT TRACKING` / `CACHING` / `GETREDIR` / `TRACKINGINFO` | 6.0 / 6.2 | 客户端缓存追踪；失效经 Pub/Sub `__redis__:invalidate`（需 REDIRECT） |
| `tracking-table-max-keys` CONFIG | 6.0 | TrackingTable 上限（0=不限，默认 1000000）；支持 GET/SET/REWRITE |
| `INFO`/`MEMORY STATS` tracking 字段 | 6.0 / 6.2 | `tracking_clients`、`tracking_total_*` 与 `clients.tracking.table.*` |
| `QUIT` 正式注册 | 1.0 | 进入命令表，可被 `COMMAND INFO`/`COUNT` 看到；Pub/Sub 与未认证场景仍可退出 |
| `XINFO CONSUMERS` 的 `inactive` | 7.2 | 距上次成功投递的毫秒；尚未成功投递为 -1；`idle` 仍按上次尝试交互 |
| `UNLINK` / `SWAPDB` / `WAIT` / `ROLE` 回归 | 4.0 / 4.0 / 3.0 / 2.8 | 异步删除、库交换、副本确认等待与角色查询；实现早已存在，bp16 补黑盒覆盖 |

ACL 停在 Redis 6：一个用户一套权限，支持 `on/off`、`>password`（SHA1 摘要保存）、`+@category/-@category`、`+cmd/-cmd`、`~keyglob`，并提供 `ACL CAT/USERS/GETUSER/GENPASS/DRYRUN`、`ACL LOG` 与 `ACL SAVE`/`ACL LOAD`。没有 Redis 7 的多选择器，也没有按键的读写权限位。频道规则已经生效：`&pattern` 按通配符匹配字面频道，`PSUBSCRIBE` 要求与 ACL 模式字面一致，`allchannels` 放行全部频道；新建用户默认 `resetchannels`，`default` 用户默认 `allchannels`。`requirepass` 成为 `default` 用户的口令，设置后未认证连接被拒绝，`CONFIG SET requirepass` 会同步更新该用户；不设置时连接以 `default` 用户进入。`ACL WHOAMI` 对任何已认证用户放行。删除用户或从 ACL 文件移除用户时，仍以该用户认证的连接会被断开，避免悬空指针。脚本里调用的命令、键与频道同样受调用者权限约束。

代码参照 Redis `7.0.15` 与 `7.2.5`（BSD）中的相关命令语义，按旧版 API 手工适配；没有引入 Redis 8 的源码或模块。

## 持久化适配

- 保留原有 RDB 格式、单文件 AOF、Windows 后台保存机制。
- 流的 RDB 仍使用旧格式，`entries-added` 与 `max-deleted-entry-id` 不落快照：RDB 恢复后前者等于当前长度、后者归零；AOF 重放和重写通过带 `ENTRIESADDED`/`MAXDELETEDID` 的 `XSETID` 保留这两个计数。
- 成功的带 TTL 的 SET 在 AOF 中记录为普通 SET + 绝对 PEXPIREAT；KEEPTTL 记录实际保留的截止时间。
- GETEX 的写入记录为 PEXPIREAT、PERSIST 或 DEL/UNLINK；GETDEL 记录为 DEL。
- 条件过期仅记录成功结果的绝对截止时间，恢复时不重新判断 NX/XX/GT/LT。
- 不迁入新版 RDB 格式、多段 AOF、RESP3、线程 I/O、TLS、Functions 或集群功能。ACL 只做到 Redis 6 的单权限集，没有 Redis 7 的选择器。`COMMAND DOCS` 用 `help.h` 的扁平字段近似上游文档图，不含完整参数树与 complexity/history。原底座已有功能保留，但本版没有对其做现代化升级。
- 没有实现 Redis 7 的客户端内存驱逐（`maxmemory-clients`）。`CLIENT NO-EVICT` 只为客户端兼容而接受并记录标志，内存压力下不会据此保留连接。
- `WAITAOF` 没有 7.2 的异步 fsync 完成队列：开启 AOF 时在命令内调用 `flushAppendOnlyFile` 并 `fsync`，然后立即返回；`numreplicas` 只做参数校验，回复中的副本数恒为 0，不阻塞到超时。
- `XINFO CONSUMERS` 的 `inactive`/`active_time` 是运行时数据，不写入本版 RDB：快照恢复后 `inactive` 回到 -1，直到再次成功投递。
- 命令延迟直方图是运行时数据，不写入 RDB/AOF；使用固定的对数桶而不是上游的 HDR 直方图，`INFO latencystats` 的百分位取桶上界。
- `SCRIPT FLUSH ASYNC` 只接受选项，脚本缓存仍同步清空；本版没有异步释放脚本缓存的线程。
- 网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。

## 性能预期

本版主要减少多步操作的网络往返，并提供单命令原子性。SMISMEMBER/ZMSCORE 可将 N 条查询合为一条；收益受批量大小、网络延迟和客户端流水线方式影响。不能据此宣称整体吞吐与 Redis 6/7/8 相同，也没有更换内存分配器、数据编码或通讯层。

不能用版本号判断完整兼容性。没有合入的新版命令仍会返回 unknown command；跨版本的数据迁移应先备份，再验证所用数据类型与格式。

## 构建

需要 Windows x64、MSVC C++ Build Tools 和 Windows SDK。本机安装的是 VS2022 Build Tools（MSVC 14.44，工具集 v143），工程文件已从 v142 改为 v143。构建脚本默认工具位置为 `D:\BuildTools\VS2022`，可用参数覆盖。

```powershell
.\build-tools\build-windows.ps1 -BuildTools 'D:\BuildTools\VS2022' -Toolset 'v143'
```

产物位于 `msvs/x64/Release`。编译脚本会生成 `src/release.h`，写入底座 Git SHA 和工作区 dirty 状态；不自动提交或创建 Git 标签。

## 验证

使用 Python 标准库的黑盒测试，启动独立临时实例，不连接现有业务 Redis：

```bash
python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe
```

覆盖新命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟及 RDB/AOF 重启。本轮补齐持久化/复制边角：UNLINK、SWAPDB、WAIT、ROLE；并保留 bp15 可观测边角及更早用例。

这是一轮选择性回移，不等于已完成上游所有安全补丁或生产压力验证。生产投放仍需在业务负载下检验 Redisson 调用、后台持久化、异常断电恢复和长时间运行。

## 来源与许可证

- Windows 底座：https://github.com/tporadowski/redis/releases/tag/v5.0.14.1
- 命令参考：https://github.com/redis/redis/tree/7.0.15
- 随包保留 `COPYING` 及原有源码版权声明。
