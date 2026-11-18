# Redis for Windows 5.0.14.1-bp42

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp41 之上加入 Redis 6.2 的 `FAILOVER`：
主库可以主动把角色交给副本，而不是等它失联。
`redis-server --version` 报告 `v=5.0.14.1-bp42`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp41 的改动

- 新增 `FAILOVER [TO host port] [FORCE] [ABORT] [TIMEOUT ms]`，非集群主库可用；副本与集群模式拒绝执行。
- 执行后主库暂停普通客户端写命令（`CLIENT_PAUSE_WRITE`，复制链路不受影响），等待目标副本 ACK 追平
  `master_repl_offset`；不带 `TO` 时自动选择已经追平的在线副本。
- 追平后主库以目标副本身份发起 `PSYNC <replid> <offset> FAILOVER`；目标副本校验第四个参数与 replid 后
  自我提升为主，随后按正常 PSYNC 处理。
- `ABORT` 取消并恢复写入；`TIMEOUT` 到期或目标掉线同样回到 `no-failover`；`FORCE` 必须与 `TO`、
  `TIMEOUT` 一起使用，允许在未追平时切换。
- `INFO replication` 的 `master_failover_state` 报告 `no-failover`、`waiting-for-sync` 或 `in-progress`；
  `PSYNC` 的 arity 放宽到 -3 以接受第四个 `FAILOVER` 参数。

## 与 Redis 6.2 的差异

- 只实现单机主从下的主动切换；集群模式继续使用 `CLUSTER FAILOVER`。
- 没有 Redis 7 中 `FAILOVER` 与 `CLUSTER FAILOVER` 共享的状态机，也没有切换期间的额外命令限制。
- 旧主库退位后按本版复制实现继续同步；切换瞬间未确认的写入由 `FORCE` 语义决定是否丢失。

## 验证

`--match failover` 通过：无进行中切换时 `ABORT` 报错；非法 `TO`、单独 `FORCE`、副本上执行均报错；
副本滞后时进入 `waiting-for-sync`，`ABORT` 后恢复可写；`TIMEOUT` 到期自动恢复；
真实切换后副本变主、旧主变从并指向新主端口，写入继续传播，双方状态回到 `no-failover`；无 `TO` 时自动选目标。

未提交、未 push、未打包、未发布。
