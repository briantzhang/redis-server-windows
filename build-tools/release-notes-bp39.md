# Redis for Windows 5.0.14.1-bp39

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp38 之上补上停机等待与无盘复制并发上限：
`shutdown-timeout` 和 `repl-diskless-sync-max-replicas`。
`redis-server --version` 报告 `v=5.0.14.1-bp39`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp38 的改动

- 新增 `shutdown-timeout`（毫秒，默认 10000）：`SHUTDOWN` 在退出前等待副本把已写命令 ACK 到当前偏移量，
  超时后继续退出。`SAVE`/`NOSAVE`、`NOW`、`ABORT` 可以组合，`NOW` 跳过等待，`ABORT` 取消待执行的关闭。
- 新增 `repl-diskless-sync-max-replicas`（默认 0 表示不限制）：一次无盘子进程最多服务的副本数，
  超出的副本留到下一轮全量同步，避免单个子进程同时向过多套接字写 RDB。
- 两个配置都支持 `CONFIG GET/SET` 与 `CONFIG REWRITE`；负数与非法值被拒绝，原值保留。
- `INFO` 字段不变，行为只影响停机和无盘全量同步的调度。

## 与 Redis 7 的差异

- `SHUTDOWN` 等待的是副本 ACK 偏移量，不等待副本 AOF 落盘；没有 `WAITAOF`。
- 无盘同步仍在主进程里 fork/复制子进程（Windows 上是 QFork），没有 Redis 7 的 socket 直写优化。
- 0 表示不限制，与 Redis 7 的语义一致。

## 验证

`--match shutdown_timeout` 通过：有副本在线时 `SHUTDOWN` 等到 ACK 后才退出，副本滞后时按超时退出，
`SHUTDOWN NOW` 不等待。`--match ops_config` 覆盖两个配置的默认值、运行时修改、非法值与 `CONFIG REWRITE`。

未提交、未 push、未打包、未发布。
