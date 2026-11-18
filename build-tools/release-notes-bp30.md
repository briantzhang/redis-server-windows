# Redis for Windows 5.0.14.1-bp30

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp29 之上放开 RESP3 订阅态的命令限制。
`redis-server --version` 报告 `v=5.0.14.1-bp30`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp29 的改动

- 协议 3（`HELLO 3`）的订阅连接可以继续执行任意普通命令，不再只允许 `(P|S)SUBSCRIBE`/`UNSUBSCRIBE`、`PING`、`QUIT` 和 `RESET`。
- 订阅确认、消息和失效仍以 `>` push 帧发送，客户端可以按帧类型把推送和命令回包分开。
- 协议 2 的订阅连接保持原有行为：仍只允许订阅管理命令和 `PING`/`QUIT`/`RESET`，尝试其它命令返回原来的错误。
- `PING` 只在协议 2 的订阅态返回 `["pong", <message>]` 数组；协议 3 的订阅态返回普通的 `+PONG`。
- `CLIENT TYPE` 对两种协议的订阅连接仍报告 `pubsub`。

## 与 Redis 7 的差异

- 只放开命令执行，订阅相关回包格式沿用本版 bp26 的 push 实现。
- 没有 Redis 7.0 之后新增的订阅态差异项（例如 `RESET` 之外的连接状态清理细节）。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 93 项测试通过，新增：
- RESP3 订阅连接上执行 `SET`/`GET`/`INCR`/`DEL` 和 `PING` 均正常。
- 同一连接仍收到 `>` push 帧形式的频道消息，推送之后命令继续可用。
- `UNSUBSCRIBE` 后连接恢复普通状态。
- RESP2 订阅连接执行 `GET` 仍被拒绝，`PING` 仍返回数组形式。

未提交、未 push、未打包、未发布。
