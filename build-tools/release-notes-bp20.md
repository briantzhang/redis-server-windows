# Redis for Windows 5.0.14.1-bp20

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp20`。

本版在 bp19 之上让 `maxmemory-clients` 真正生效。

## 相对 bp19 的改动

### 客户端内存驱逐

- 新增配置 `maxmemory-clients`，单位字节，默认 0 表示关闭。支持 `CONFIG GET`、`CONFIG SET` 和 `CONFIG REWRITE`。
- 每次事件循环结束时统计普通客户端占用的查询缓冲、回复缓冲和已解析参数向量。总和超过限额后，按占用从大到小断开连接，直到回到限额以内或没有可断开的连接。
- 带 `CLIENT NO-EVICT` 标志的连接，以及副本、主连接和 `MONITOR` 连接，不参与统计，也不会被断开。
- `INFO clients` 增加 `maxmemory_clients` 和 `evicted_clients`。

占用按上述三项估算，不是 Redis 7 里按客户端内存桶分摊的算法，所以同一个负载下断开的连接数量可能与上游不同。

## 不包含

RESP3、Functions、TLS、多段 AOF、新版 RDB、集群、Sentinel。命令解析与执行仍在主线程。没有引入 Redis 8 源码。只有 Windows x64。

## 验证

80 项黑盒回归全部通过，其中客户端用例覆盖：超限连接被断开、`CLIENT NO-EVICT` 的连接保留、`evicted_clients` 计数增加。测试使用独立临时目录和随机端口，不连接现有实例。
