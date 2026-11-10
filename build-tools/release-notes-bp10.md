# Redis for Windows 5.0.14.1-bp10

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp10`。

本版在 bp9 的基础上补齐命令延迟直方图、ACL 频道权限与脚本内权限检查，并接受 `SCRIPT FLUSH` 的同步/异步选项。

## 相对 bp9 新增

### 延迟统计

- `LATENCY HISTOGRAM [command ...]`：返回每条命令的调用次数和延迟直方图。样本以纳秒记录，从 1024ns 起按 2 的幂分桶，回复里的 `histogram_usec` 是“微秒桶上界 → 累计次数”的扁平数组（RESP2 map 编码）。没有样本的命令不出现。
- `INFO latencystats`：每条有样本的命令输出 `p50`、`p99`、`p99.9`，单位微秒。百分位取桶上界，适合看量级。
- `latency-tracking yes|no`：控制是否采集，默认 `yes`。关闭后不再新增样本，已有直方图保留到下一次重置。
- `LATENCY RESET` 与 `CONFIG RESETSTAT` 都会清空全部命令直方图；`LATENCY RESET` 的返回值仍是重置的事件数。

### ACL 频道权限

- `ACL SETUSER ... &pattern`、`allchannels`、`&*`、`resetchannels` 现在真正生效，作用于 `SUBSCRIBE`、`PSUBSCRIBE`、`PUBLISH`、`SSUBSCRIBE`、`SPUBLISH`。
- 字面频道用通配符匹配；`PSUBSCRIBE` 的模式必须与某条 ACL 模式字面一致，避免用更宽的模式订阅权限外的频道。
- 一次带多个频道的 `SUBSCRIBE` 会先检查全部参数，任一越权就整体拒绝，不会产生部分订阅。
- 拒绝时回复 `NOPERM this user has no permissions to access one of the channels used as arguments`，并在 `ACL LOG` 里记一条 `reason=channel`。
- `ACL GETUSER` 的 `channels` 字段返回 `&*` 或具体模式；`ACL LIST`、`ACL SAVE` 输出 `allchannels`/`resetchannels` 和 `&pattern`，`ACL LOAD` 会恢复。
- 新建用户默认 `resetchannels`；`default` 用户默认 `allchannels`。在 `allchannels` 状态下再添加 `&pattern` 会报错，需先 `resetchannels`。

### 脚本内 ACL

- `EVAL`/`EVALSHA` 里的 `redis.call` 现在按调用者的用户检查命令、键和频道。越权时脚本收到 `NOPERM` 错误，不会执行该命令。拒绝记录带 `context=lua`。

### 脚本缓存

- `SCRIPT FLUSH [ASYNC|SYNC]` 接受两个选项。本版没有异步释放脚本缓存的线程，两种写法都同步清空缓存。

## bp9 已有的能力

`ACL LOG`/`ACL SAVE`/`ACL LOAD` 与 `aclfile`、`CLIENT SETINFO`、`XGROUP SETID ENTRIESREAD` 与 `XINFO GROUPS` 的 `entries-read`/`lag`、`INFO errorstats`、分片 Pub/Sub；bp8 的 `GEOSEARCH`/`GEOSEARCHSTORE`、ACL 查询、`CLIENT KILL` 过滤、`INFO everything`、`WAITAOF`、`SHUTDOWN` 选项；以及 bp1–bp7 的 `SET` 系列、`GETEX`/`GETDEL`、条件过期、位范围、只读入口、`CLIENT PAUSE`、多模式 `CONFIG`、`COMMAND LIST`、流命令、多键弹出、有序集合运算、`COPY`、`RESET` 等。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐、`COMMAND DOCS`。ACL 不含 Redis 7 的选择器与按键读写权限位；延迟直方图不是上游的 HDR 实现，`INFO latencystats` 的百分位取桶上界。`SCRIPT FLUSH ASYNC` 只接受选项。命令语义参照 Redis `7.0.15`、`7.2.5` 与 `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

68 项黑盒回归全部通过，其中新增 4 项：延迟直方图与 `INFO latencystats`、`CONFIG RESETSTAT`/`LATENCY RESET` 两种重置和 `latency-tracking` 开关；ACL 频道权限的字面订阅、模式订阅、发布、分片频道、`ACL GETUSER`/`ACL LIST` 与 `ACL LOG`；脚本内命令、键和频道越权；`SCRIPT FLUSH ASYNC`/`SYNC` 与非法参数。`ACL SAVE`/`LOAD` 用例扩展为保留频道规则。测试使用独立临时目录和随机端口，不连接现有实例。
