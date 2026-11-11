# Redis for Windows 5.0.14.1-bp14

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp14`。

本版在 bp13 的 CLIENT TRACKING 之上补齐运维可见性与收尾。

## 相对 bp13 新增

### TRACKING 运维收尾

- `CONFIG GET/SET/REWRITE tracking-table-max-keys`（默认 1000000，0 表示不限制）
- `INFO clients` 增加 `tracking_clients`
- `INFO stats` 增加 `tracking_total_keys` / `tracking_total_items` / `tracking_total_prefixes`
- `MEMORY STATS` 增加 `clients.tracking.table.keys|items|prefixes`
- `CLIENT LIST` 中 tracking 客户端继续带 `t` / `R` 标志（bp13 已有）
- 回归覆盖 `CLIENT TRACKING ON BCAST PREFIX ... REDIRECT`

### 运维补齐

- `QUIT` 正式进入命令表（`COMMAND INFO quit` / `COMMAND COUNT` 可见）；Pub/Sub 上下文与未认证连接仍可退出
- 确认并测试 `SCRIPT HELP`、`COMMAND INFO`

## bp13 已有的能力

`CLIENT TRACKING` / `CACHING` / `GETREDIR` / `TRACKINGINFO`（RESP2 Pub/Sub 失效）；bp12 的 `SCAN TYPE`、`HELLO`、`XINFO CONSUMERS inactive`；以及更早各轮能力。

## 不包含

RESP3（`HELLO 3` 被拒绝）、RESP3 invalidate push、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐。ACL 不含 Redis 7 的选择器。命令语义参照 Redis `7.0.15`、`7.2.5` 与 `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

黑盒回归覆盖 BCAST+PREFIX、tracking CONFIG/INFO/MEMORY、QUIT/SCRIPT HELP/COMMAND INFO，以及既有 bp13 及更早用例。测试使用独立临时目录和随机端口，不连接现有实例。
