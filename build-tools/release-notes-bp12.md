# Redis for Windows 5.0.14.1-bp12

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp12`。

本版在 bp11 的基础上补齐键扫描类型过滤、客户端握手，以及流消费者 inactive 计时。

## 相对 bp11 新增

### SCAN TYPE

- `SCAN cursor [MATCH pattern] [COUNT count] [TYPE type]`：只返回指定类型的键。
- 支持 `string`/`list`/`set`/`zset`/`hash`/`stream`（大小写不敏感）。
- 未知类型名不报错，本轮扫描结果为空（与 Redis 6/7 在 8.0 之前的行为一致）。
- `TYPE` 仅对 `SCAN` 有效；出现在 `HSCAN`/`SSCAN`/`ZSCAN` 中会报 syntax error。
- 过滤使用 `LOOKUP_NOTOUCH`，不更新键的 LRU/LFU。

### HELLO（仅 RESP2）

- `HELLO [protover [AUTH username password] [SETNAME clientname]]`：客户端握手。
- 无参数或 `protover=2` 接受；`protover=3` 或其它值返回 `NOPROTO`。本版没有 RESP3。
- 回复是 RESP2 扁平字段数组：`server`/`version`/`proto`/`id`/`mode`/`role`/`modules`（modules 恒为空数组）。
- `AUTH` 走与 `AUTH` 命令相同的 ACL 校验；失败返回 WRONGPASS，并记入 ACL LOG。
- `SETNAME` 规则与 `CLIENT SETNAME` 相同。
- 配置了 `requirepass` 时，未认证连接可调用 `HELLO`（以便带 `AUTH`），其它命令仍被拒绝。

### XINFO CONSUMERS inactive

- 每个消费者新增 `inactive`：距上次**成功投递**条目的毫秒数；尚未成功投递时为 `-1`。
- `idle` 含义不变：距上次**尝试**交互（`XREADGROUP`/`XCLAIM`/`XAUTOCLAIM`，含空读）的毫秒数。
- `XINFO STREAM ... FULL` 的消费者块新增 `active-time` 绝对时间戳。
- `active_time` 不写入本版 RDB；快照恢复后 `inactive` 回到 `-1`，直到再次成功投递。

## bp11 已有的能力

`COMMAND DOCS`、`ZRANK`/`ZREVRANK WITHSCORE`、`ACL HELP`/`WHOAMI`；bp10 的延迟直方图与 ACL 频道权限；以及更早各轮的 GEOSEARCH、流命令、多键弹出、COPY、条件过期等。

## 不包含

RESP3（`HELLO 3` 被拒绝）、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐。ACL 不含 Redis 7 的选择器。命令语义参照 Redis `7.0.15`、`7.2.5` 与 `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

72 项黑盒回归全部通过，其中本轮覆盖：`SCAN TYPE` 过滤与非法用法；`HELLO` 握手、`SETNAME`、拒绝协议 3、带 `requirepass` 的 `AUTH`；`XINFO CONSUMERS inactive` 与 FULL 中的 `active-time`。测试使用独立临时目录和随机端口，不连接现有实例。
