# Redis for Windows 5.0.14.1-bp56

本轮补齐 `CLIENT UNBLOCK` 的可观测回归：

- 覆盖 TIMEOUT 模式：被阻塞的 BLPOP 收到与超时一致的 nil 回复。
- 覆盖 ERROR 模式：被阻塞的 BLPOP 收到 `-UNBLOCKED`，连接仍可继续发送 PING。
- 覆盖不存在的客户端 ID 返回 0，以及非法 reason 返回错误。
- 回归新增 1 项；全量共 126 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
