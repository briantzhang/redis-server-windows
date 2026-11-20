# Redis for Windows 5.0.14.1-bp84

本次修复并补强 RDB/RESP3/Functions 行为：

- 修复 `sanitize-dump-payload` 校验 RDB 11 stream consumer 时覆盖 PEL 长度的问题，当前带 consumer 的 stream DUMP 可正常 RESTORE。
- HELLO 回复增加 `username` 字段，并更新 HELP 中 RESP2/RESP3 支持说明。
- FUNCTION KILL 接入现有 Lua kill 状态机；FUNCTION STATS 在执行期间报告运行函数信息。
- 新增 stream sanitize、HELLO username 和 FUNCTION KILL 空闲路径回归。

全量 Windows 原生回归 144 项通过。
