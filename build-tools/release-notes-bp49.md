# Redis for Windows 5.0.14.1-bp49

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版。bp49 让流计数进入 RDB 快照。
`redis-server --version` 报告 `v=5.0.14.1-bp49`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp48 的改动

- 在保持 RDB 版本 9 的前提下，用兼容长度标记把流的 `entries-added`、`max-deleted-entry-id`
  和消费组 `entries_read` 写入快照；旧版快照仍按原有规则恢复计数。
- RDB 加载、DUMP/RESTORE 的 payload 预校验都识别新字段，未知格式按损坏数据处理。
- 新增回归用例：SAVE/重启后的流计数与消费组 lag，以及开启 `sanitize-dump-payload yes`
  后的 DUMP/RESTORE 往返。

## 验证

流与 sanitize 相关 9 项回归通过；新增用例单独通过；全量回归共 119 项，日志末尾为 `OK`。
未提交、未 push、未打包、未发布。
