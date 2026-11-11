# Redis for Windows 5.0.14.1-bp15

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp15`。

本版在 bp14 之上补齐运维可观测与若干边角回归；命令实现本身此前大多已存在，本轮以黑盒覆盖为主。

## 相对 bp14 新增

### 运维可观测回归

- `LATENCY DOCTOR` / `GRAPH` / `LATEST` / `HISTORY` / `HELP`（配合 `latency-monitor-threshold`）
- `MEMORY DOCTOR` / `MALLOC-STATS` / `PURGE` / `HELP`
- `SLOWLOG HELP` / `GET` / `LEN` / `RESET`

### DUMP / RESTORE 全选项

- `DUMP` ↔ `RESTORE` 往返
- `REPLACE`、`ABSTTL`、`IDLETIME`；拒绝 `IDLETIME`+`FREQ` 同用与非法 `FREQ`

### CLIENT 连接边角

- `CLIENT REPLY OFF|SKIP|ON`
- `CLIENT UNBLOCK <id>` 与 `ERROR` 原因

### OBJECT / PUBSUB 补测

- `OBJECT HELP` / `ENCODING` / `REFCOUNT` / `IDLETIME`；非 LFU 策略下 `FREQ` 报错
- `PUBSUB HELP` / `CHANNELS` / `NUMSUB` / `NUMPAT`

## bp14 已有的能力

`tracking-table-max-keys` CONFIG、`INFO`/`MEMORY` tracking 统计、BCAST+PREFIX、`QUIT` 入表；以及 bp13 的 `CLIENT TRACKING`（RESP2 Pub/Sub 失效）等更早能力。

## 不包含

RESP3、Functions、TLS、线程化 I/O、多段 AOF、新版 RDB、集群、Sentinel、客户端内存驱逐。没有引入 Redis 8 源码。只有 Windows x64。

## 验证

黑盒回归覆盖上述四组用例，并保留 bp14 及更早测试。使用独立临时目录和随机端口，不连接现有实例。
