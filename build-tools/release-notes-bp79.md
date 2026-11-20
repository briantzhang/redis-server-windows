# 5.0.14.1-bp79

本版建立旧 RDB 与 DUMP 兼容矩阵。

- 新增 DUMP footer 版本回归：当前版本 11 的 payload 可正常 RESTORE，改写为版本 10 并重算 CRC64 后仍可恢复；版本 12 被拒绝。
- 明确本地 RDB 版本 11 仅包含流消费者 `active_time` 扩展，旧版本加载仍按原字段集合恢复。
- Windows x64 Release 构建通过，141 项回归全部通过。

未 push，未发布。
