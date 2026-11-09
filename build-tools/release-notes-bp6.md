# Redis for Windows 5.0.14.1-bp6

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp6`。

本版在 bp5 的基础上补齐 Redis 6.2 的流命令能力，并为流对象增加两个计数。

## 相对 bp5 新增

### 流裁剪

- `XADD key [NOMKSTREAM] [MAXLEN|MINID [~|=] threshold [LIMIT count]] <ID|*> field value ...`：追加时可以按最小 ID 裁剪，`LIMIT` 限制单次删除的条目数（只对 `~` 近似裁剪有效，按 listpack 节点计）。
- `XTRIM key MAXLEN|MINID [~|=] threshold [LIMIT count]`：支持 `MINID` 和 `LIMIT`；同时给出 `MAXLEN` 与 `MINID`、或 `LIMIT` 不带 `~` 时报语法错误。
- 近似裁剪在传播给 AOF/副本前改写成确定形式：`MAXLEN ~ n` 改写为 `MAXLEN = <裁剪后长度>`，`MINID ~ id` 改写为 `MINID = <第一条存活 ID>`（流为空时用最大 ID，保证重放后同样清空）。

### 流命令

- `XADD ... NOMKSTREAM`：键不存在时返回空值且不创建键；键存在时照常追加。与 `MAXLEN`/`MINID` 选项可以同时出现。
- `XGROUP CREATECONSUMER key group consumer`：显式创建消费者，已存在返回 0、新建返回 1。`XREADGROUP ... NOACK` 新建消费者时也会传播这条命令，否则 AOF 重放和副本上会缺少该消费者。
- `XSETID key id [ENTRIESADDED n] [MAXDELETEDID id]`：可同时校正 `entries-added` 与 `max-deleted-entry-id`，并校验新 ID 不小于流顶条目、`entries_added` 不小于当前长度、`max-deleted-entry-id` 不大于新 ID。
- `XPENDING key group [IDLE min-idle-time] start end count [consumer]`：按空闲时间过滤待处理消息。
- `XAUTOCLAIM key group consumer min-idle-time start [COUNT count] [JUSTID]`：批量把 PEL 条目转给指定消费者，回复 `[下一游标, 已认领条目, 已清理的已删除 ID]`；游标为 `0-0` 表示扫描完毕。流条目已删除的 PEL 记录会被移除并报告；`JUSTID` 只返回 ID 且不增加投递次数。支持 `(id` 开区间起点。
- `XINFO STREAM key [FULL [COUNT n]]`：摘要新增 `max-deleted-entry-id`、`entries-added`、`recorded-first-entry-id`；`FULL` 额外输出条目、组、消费者及各自的 PEL，`COUNT` 限制条目和 PEL 的返回数量，`0` 表示不限制。

### 计数与持久化

- 流对象内部记录 `entries-added`（历史追加总数）和 `max-deleted-entry-id`（`XDEL` 删除过的最大 ID）；`XINFO STREAM` 可见，`COPY` 会复制。
- AOF 重写在 `XSETID` 中带上 `ENTRIESADDED` 与 `MAXDELETEDID`，重放后计数不丢。
- RDB 仍使用旧流格式，这两个计数不写入快照：RDB 恢复后 `entries-added` 等于当前长度、`max-deleted-entry-id` 归零。依赖计数的场景应使用 AOF 恢复，或恢复后手工 `XSETID` 校正。

## bp5 已有的能力

`LMPOP`/`BLMPOP`、`ZMPOP`/`BZMPOP`、`CLIENT NO-TOUCH`/`NO-EVICT`；bp4 的 `LPOP`/`RPOP` 的 `count`、`BLMOVE`、统一 `ZRANGE`/`ZRANGESTORE`；bp3 的 `ZADD ... GT|LT`、`HRANDFIELD`、`ZRANDMEMBER`；bp2 的 `LPOS`、`LMOVE`、`SINTERCARD`、`ZINTERCARD`、lazyfree 配置；bp1 的 `SET ... KEEPTTL/GET/EXAT/PXAT`、`GETEX`、`GETDEL`、过期命令的 `NX/XX/GT/LT`、`EXPIRETIME/PEXPIRETIME`、`SMISMEMBER`、`ZMSCORE`、`ZUNION`/`ZINTER`/`ZDIFF`/`ZDIFFSTORE`、`COPY`、`RESET` 和 Redis 6 范围的 ACL。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐。ACL 不含 Redis 7 的选择器与按键读写权限位。持久化仍是底座的单文件 AOF 和原有 RDB；网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。命令语义参照 Redis `7.0.15` 与 `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

41 项黑盒回归全部通过，其中新增 8 项流用例，覆盖 `MINID`/`LIMIT` 裁剪（含跨 listpack 节点）、`NOMKSTREAM`、`CREATECONSUMER`、`XPENDING IDLE`、`XAUTOCLAIM`（游标、JUSTID、已删除 PEL 条目、开区间）、`XSETID` 计数选项与错误路径、`XINFO STREAM FULL`，以及 AOF 重放/重写后的计数和 NOACK 消费者恢复。测试使用独立临时目录和随机端口，不连接现有实例。
