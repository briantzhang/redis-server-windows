# Redis for Windows 5.0.14.1-bp26

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp25 之上补齐 RESP3 的 push 帧。
`redis-server --version` 报告 `v=5.0.14.1-bp26`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp25 的改动

- 新增 `addReplyPushLen()`：协议 3 输出 `>` push 头，协议 2 继续输出 `*` 数组头。
- Pub/Sub 的订阅确认、退订确认、`message`、`pmessage`、分片订阅确认与 `smessage` 在 RESP3 下改为 push 帧。
- `CLIENT TRACKING` 的失效通知在 RESP3 连接上直接以 `[invalidate, key]` push 发送，不再要求 `REDIRECT`。
- BCAST 失效的 push 第二项是本次涉及键的数组；`FLUSHALL`/`FLUSHDB` 的第二项是 null。
- RESP2 行为保持不变：Pub/Sub 仍是普通数组，TRACKING 失效仍投递到 `__redis__:invalidate` 频道，必须 `REDIRECT` 到已订阅该频道的连接。

## 与 Redis 6/7 的差异

- push 只用于 Pub/Sub 与客户端缓存失效；其它命令回复仍是常规类型。
- 没有 `CLIENT TRACKINGINFO` 的 RESP3 扩展字段或 `CLIENT NO-EVICT` 之外的客户端内存统计。
- 失效通知与命令回复共用同一条连接和同一发送队列，顺序按服务端产生顺序排列。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 89 项测试通过，新增：
- RESP3 下 SUBSCRIBE 确认、PUBLISH 投递和 UNSUBSCRIBE 确认均为 push 帧，RESP2 仍为数组。
- RESP3 `CLIENT TRACKING ON` 无需 REDIRECT 即收到 push 失效，BCAST + PREFIX 与 FLUSHALL 的 nil 负载均正确。

未提交、未 push、未打包、未发布。
