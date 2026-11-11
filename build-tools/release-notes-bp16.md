# Redis for Windows 5.0.14.1-bp16

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp16`。

本版在 bp15 之上补齐持久化/复制边角回归；`UNLINK`/`SWAPDB`/`WAIT`/`ROLE` 实现此前已存在，本轮以黑盒覆盖为主。

## 相对 bp15 新增

### 持久化 / 复制边角回归

- `UNLINK`：异步删除，返回实际删除的键数；缺失键计为 0
- `SWAPDB`：交换两个数据库的键空间；非法下标报错；客户端仍停在原 SELECT
- `WAIT`：无副本时立即返回 0；非法超时报错
- `ROLE`：单机主节点回复 `master`、复制偏移与空从库列表

## bp15 已有的能力

LATENCY/MEMORY/SLOWLOG 可观测、DUMP/RESTORE 全选项、CLIENT REPLY/UNBLOCK、OBJECT/PUBSUB HELP；以及 bp14 的 tracking 收尾、`QUIT` 入表等更早能力。

## 不包含

RESP3、Functions、TLS、线程化 I/O、多段 AOF、新版 RDB、集群、Sentinel、客户端内存驱逐。没有引入 Redis 8 源码。只有 Windows x64。

## 验证

黑盒回归覆盖上述四条命令，并保留 bp15 及更早测试。使用独立临时目录和随机端口，不连接现有实例。
