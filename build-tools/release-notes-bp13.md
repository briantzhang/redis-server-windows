# Redis for Windows 5.0.14.1-bp13

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp13`。

本版在 bp12 的基础上补齐 RESP2 可用的客户端缓存追踪（CLIENT TRACKING）。

## 相对 bp12 新增

### CLIENT TRACKING（RESP2 / Pub/Sub 失效）

- `CLIENT TRACKING ON|OFF [REDIRECT client-id] [BCAST] [PREFIX prefix ...] [OPTIN] [OPTOUT] [NOLOOP]`
- `CLIENT CACHING YES|NO`（仅 OPTIN/OPTOUT）
- `CLIENT GETREDIR` / `CLIENT TRACKINGINFO`
- 本版没有 RESP3 push：失效通知通过 Pub/Sub 频道 `__redis__:invalidate` 投递，因此通常需要 `REDIRECT` 到一个已 `SUBSCRIBE __redis__:invalidate` 的连接。
- 普通模式：只读命令访问过的键被记入 TrackingTable；键被修改时向追踪客户端（经 REDIRECT）发失效消息。
- `BCAST`：按前缀广播本事件循环内修改过的键；`PREFIX` 仅在 BCAST 下合法；同客户端前缀不可重叠。
- `OPTIN`/`OPTOUT` + `CACHING`：控制下一条命令是否记入追踪表。
- `NOLOOP`：不把客户端自己的写操作通知给自己。
- `FLUSHDB`/`FLUSHALL` 向追踪客户端发送 null bulk 载荷，表示全部失效。
- `tracking-table-max-keys` 默认 1000000（当前未暴露为 CONFIG 项，与代码内默认一致）。

## bp12 已有的能力

`SCAN TYPE`、`HELLO`（仅 RESP2）、`XINFO CONSUMERS inactive`；bp11 的 `COMMAND DOCS`、`ZRANK WITHSCORE`、`ACL HELP`/`WHOAMI`；以及更早各轮的延迟直方图、ACL 频道、流命令、COPY、条件过期等。

## 不包含

RESP3（`HELLO 3` 被拒绝）、RESP3 invalidate push、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐。ACL 不含 Redis 7 的选择器。命令语义参照 Redis `7.0.15`、`7.2.5` 与 `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

73 项黑盒回归全部通过，其中本轮覆盖 CLIENT TRACKING ON + REDIRECT 的 Pub/Sub 失效、OPTIN+CACHING、GETREDIR/TRACKINGINFO/OFF；以及既有 bp12 及更早用例。测试使用独立临时目录和随机端口，不连接现有实例。
