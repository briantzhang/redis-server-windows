# Redis for Windows 5.0.14.1-bp7

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp7`。

本版在 bp6 的基础上补齐一批小命令与运维命令：位范围、最长公共子序列、只读命令入口、客户端暂停、CONFIG 多参数、命令清单。

## 相对 bp6 新增

### 位操作

- `BITCOUNT key [start end [BYTE|BIT]]`：范围单位可选字节或位。按位计数时对首尾位使用掩码，负下标按所选单位换算。
- `BITPOS key bit [start [end [BYTE|BIT]]]`：同样支持 `BYTE`/`BIT`。未显式给范围且字符串全为 1 时，查找 0 返回字符串位数（与上游一致）；显式给范围时返回 -1。
- `BITFIELD_RO key GET <type> <offset> ...`：只读 BITFIELD，只接受 `GET`（`OVERFLOW` 可一起出现）；`SET`/`INCRBY` 返回错误且不改动数据。

### 字符串与排序

- `LCS key1 key2 [LEN] [IDX [MINMATCHLEN n] [WITHMATCHLEN]]`：最长公共子序列。`LEN` 只返回长度；`IDX` 返回 `matches` 区间数组与 `len`；`MINMATCHLEN` 过滤短匹配，`WITHMATCHLEN` 在每条匹配后附带长度。`LEN` 与 `IDX` 同时出现报错。
- `SORT_RO key [BY ...] [LIMIT ...] [GET ...] [ASC|DESC] [ALPHA]`：只读 SORT；出现 `STORE` 返回 `SORT_RO can't be used with STORE`，不写目标键。

### 只读脚本

- `EVAL_RO script numkeys key ...` 与 `EVALSHA_RO sha1 numkeys key ...`：脚本执行期间调用任何写命令都会被拒绝，整个调用报错且数据不变；普通 `EVAL`/`EVALSHA` 仍可写。

### 客户端暂停

- `CLIENT PAUSE timeout [WRITE|ALL]`：`WRITE` 只拦写命令与 `EXEC`，`ALL` 读写都拦；省略模式时等同 `ALL`。副本连接永不被暂停。
- `CLIENT UNPAUSE`：立即恢复，不等待超时。
- `CLIENT INFO`：返回当前连接的一行信息，格式与 `CLIENT LIST` 的行相同。

实现上移植了 Redis 6.2 的 pending-command 机制：命令先完整解析，再由 `processCommand` 决定是否阻塞；被暂停的连接挂在独立链表上，恢复后按原顺序执行，客户端不需要重发。暂停超时由原有的阻塞客户端超时扫描处理。

### 配置与命令清单

- `CONFIG GET pattern [pattern ...]`：一次查询多个模式，重叠的模式不会重复输出同一参数。
- `CONFIG SET parameter value [parameter value ...]`：一次设置多对参数，按顺序逐对应用。这不是原子操作：后面的参数非法时，前面已经生效；返回错误后应重新 `CONFIG GET` 确认当前值。参数个数不是 2 的倍数时返回参数个数错误。
- `COMMAND LIST [FILTERBY PATTERN <glob>|ACLCAT <category>|MODULE <name>]`：返回命令名。`ACLCAT` 用命令级读写标志近似 `read`/`write`/`keyspace` 分类，其他分类为空；`MODULE` 恒为空（本版不能加载模块）。
- `COMMAND GETKEYSANDFLAGS <full-command>`：返回每个键及其标志。由于 Redis 5.0 没有键规格，标志按命令级属性近似：只读命令给出 `RO`/`access`，写命令给出 `RW`/`access`/`update`。

## bp6 已有的能力

流命令补齐（`XADD`/`XTRIM` 的 `MINID`/`LIMIT`/`NOMKSTREAM`、`XGROUP CREATECONSUMER`、`XSETID` 计数选项、`XPENDING IDLE`、`XAUTOCLAIM`、`XINFO STREAM FULL` 与 `entries-added`/`max-deleted-entry-id` 计数）；bp5 的 `LMPOP`/`BLMPOP`、`ZMPOP`/`BZMPOP`、`CLIENT NO-TOUCH`/`NO-EVICT`；bp4 的 `LPOP`/`RPOP` 的 `count`、`BLMOVE`、统一 `ZRANGE`/`ZRANGESTORE`；bp3 的 `ZADD ... GT|LT`、`HRANDFIELD`、`ZRANDMEMBER`；bp2 的 `LPOS`、`LMOVE`、`SINTERCARD`、`ZINTERCARD`、lazyfree 配置；bp1 的 `SET ... KEEPTTL/GET/EXAT/PXAT`、`GETEX`、`GETDEL`、过期命令的 `NX/XX/GT/LT`、`EXPIRETIME/PEXPIRETIME`、`SMISMEMBER`、`ZMSCORE`、`ZUNION`/`ZINTER`/`ZDIFF`/`ZDIFFSTORE`、`COPY`、`RESET` 和 Redis 6 范围的 ACL。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐。ACL 不含 Redis 7 的选择器与按键读写权限位。持久化仍是底座的单文件 AOF 和原有 RDB；网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。命令语义参照 Redis `7.0.15` 与 `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

49 项黑盒回归全部通过，其中新增 8 项：`BITCOUNT`/`BITPOS` 的 `BYTE`/`BIT` 范围与边界、`BITFIELD_RO` 的只读限制、`LCS` 的 `LEN`/`IDX`/`MINMATCHLEN`/`WITHMATCHLEN`、`SORT_RO` 拒绝 `STORE`、`EVAL_RO`/`EVALSHA_RO` 拒绝写命令、`CLIENT PAUSE WRITE|ALL`/`UNPAUSE`/`INFO`（含阻塞与恢复时序）、`CONFIG GET` 多模式去重与 `CONFIG SET` 多参数（含部分生效）、`COMMAND LIST` 过滤与 `COMMAND GETKEYSANDFLAGS`。测试使用独立临时目录和随机端口，不连接现有实例。
