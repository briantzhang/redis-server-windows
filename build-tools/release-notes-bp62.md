# Redis for Windows 5.0.14.1-bp62

本轮补齐 RESP3 shard Pub/Sub 回归：

- 覆盖 `SSUBSCRIBE` 确认、`SPUBLISH` 二进制消息投递和 `SUNSUBSCRIBE` 取消。
- 验证 RESP3 下三类消息都使用 `>` push 帧；保留原有普通 Pub/Sub 的 RESP3 push 与 RESP2 数组行为。
- 全量共 131 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
