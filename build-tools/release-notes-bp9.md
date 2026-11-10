# Redis for Windows 5.0.14.1-bp9

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp9`。

本版在 bp8 的基础上补齐 ACL 审计与持久化、客户端库信息、消费组已读计数、错误统计和分片 Pub/Sub。

## 相对 bp8 新增

### ACL 审计与持久化

- `ACL LOG [count|RESET]`：记录被拒绝的命令、键访问和认证失败。每条包含 `count`、`reason`、`context`（toplevel/multi/lua）、`object`、`username`、`age-seconds`、`client-info`。相同条目在 60 秒内合并计数并移到最前；`acllog-max-len` 控制总条数（默认 128，设为 0 关闭记录）。
- `ACL SAVE` / `ACL LOAD`：把用户规则写回 `aclfile` 并重新加载。启动时若配置了 `aclfile` 会自动加载；加载失败时保留内存中的旧用户并返回错误，不会半途替换。规则文件里缺失的用户会被删除，仍以该用户认证的连接随即断开。
- `ACL LIST` 输出完整规则文本（`on/off`、`#<sha1>`、`~pattern`、命令规则），可直接写回 `aclfile`。
- `ACL DELUSER` 支持一次删除多个用户，返回实际删除数量；删除用户会断开其在线连接，避免悬空引用。
- `ACL SETUSER` 接受 `&pattern`、`allchannels`、`resetchannels` 但不生效（本版 ACL 停在 Redis 6 的单权限集）。
- `CONFIG SET requirepass` 同步更新 `default` 用户的口令。

### 客户端与消费组

- `CLIENT SETINFO lib-name <name>` / `CLIENT SETINFO lib-ver <ver>`：登记客户端库信息，长度上限 128，只接受可打印 ASCII；`CLIENT LIST`/`CLIENT INFO` 显示 `lib-name=`、`lib-ver=`。
- `XGROUP SETID key group id [ENTRIESREAD n]`：记录消费组已读条目数，负数被拒绝。`XINFO GROUPS` 输出 `entries-read` 与 `lag`；`XINFO STREAM FULL` 的组信息同步增加。阻塞中的 `XREADGROUP` 成功投递后也会累加已读计数。

### 统计与 Pub/Sub

- `INFO errorstats`：按错误码统计回复数量（如 `errorstat_WRONGTYPE:count=1`），包含共享错误对象；`CONFIG RESETSTAT` 清零。
- `SSUBSCRIBE` / `SUNSUBSCRIBE` / `SPUBLISH` 与 `PUBSUB SHARDCHANNELS [pattern]` / `PUBSUB SHARDNUMSUB [channel ...]`：分片频道使用独立的订阅字典，与普通频道互不影响；`RESET` 会同时退订两类频道。单机下按本节点投递，`CLIENT LIST` 的 `ssub=` 给出分片订阅数。

## bp8 已有的能力

`GEOSEARCH`/`GEOSEARCHSTORE`、`ACL CAT/USERS/GETUSER/GENPASS/DRYRUN`、`CLIENT KILL` 的 `LADDR`/`USER`/`MAXAGE`、`CLIENT LIST` 的 `laddr=`/`user=`、`INFO everything`、`WAITAOF`、`SHUTDOWN` 选项；bp7 的位范围与只读入口、`CLIENT PAUSE`、多模式 `CONFIG`、`COMMAND LIST`/`GETKEYSANDFLAGS`；bp6 的流命令补齐；bp5 的多键弹出与客户端标志；bp4 的批量弹出与统一 `ZRANGE`；bp3 的条件分数更新与随机取样；bp2 的 `LPOS`/`LMOVE`/交集基数/lazyfree；bp1 的 `SET` 系列选项、`GETEX`/`GETDEL`、过期命令条件、`EXPIRETIME`/`PEXPIRETIME`、`SMISMEMBER`/`ZMSCORE`、有序集合运算、`COPY`、`RESET` 和 Redis 6 范围的 ACL。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐、`LATENCY HISTOGRAM`、`COMMAND DOCS`。ACL 不含 Redis 7 的选择器与按键读写权限位；`WAITAOF` 不含副本 AOF 确认与阻塞等待。持久化仍是底座的单文件 AOF 和原有 RDB；网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。命令语义参照 Redis `7.0.15`、`7.2.5` 与 `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

64 项黑盒回归全部通过，其中新增 9 项：`ACL LOG` 的分组与上限及 `RESET`；无 `aclfile` 时 `ACL SAVE`/`LOAD` 报错；`aclfile` 的启动加载、保存后重载与损坏文件保留旧用户；`CLIENT SETINFO` 与 `CLIENT LIST` 字段；`XGROUP SETID ENTRIESREAD` 与 `XINFO GROUPS` 的 `entries-read`/`lag`；`INFO errorstats` 计数与 `CONFIG RESETSTAT` 清零；分片 Pub/Sub 的订阅、发布、退订、`PUBSUB` 查询与 `RESET`；`ACL DELUSER` 断开在线连接；`requirepass` 错误口令拒绝与运行期轮换。测试使用独立临时目录和随机端口，不连接现有实例。
