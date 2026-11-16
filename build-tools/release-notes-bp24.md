# Redis for Windows 5.0.14.1-bp24

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp23 之上加入 Redis Functions 兼容层。
`redis-server --version` 报告 `v=5.0.14.1-bp24`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp23 的改动

- 新增 `FUNCTION LOAD | DELETE | FLUSH | LIST | STATS | DUMP | RESTORE | KILL | HELP`。
- 新增 `FCALL` / `FCALL_RO`，函数库用 `#!lua name=<库名>` 声明名称，通过 `redis.register_function()` 注册 Lua 回调。
- 回调按 `(KEYS, ARGV)` 两个表调用；`no-writes` 标记和 `FCALL_RO` 会拒绝写命令。
- RDB 以 AUX 字段保存函数源码；AOF rewrite 在函数命令之前写出 `FUNCTION LOAD`，重启后可恢复。
- `FUNCTION LIST` 和 `FUNCTION STATS` 使用已有的 RESP2/RESP3 编码接口。
- 集群模式沿用底座的 Redis 5.0 集群实现，本版补三节点黑盒覆盖和文档；集群总线端口是客户端端口 +10000。

## 与 Redis 7 的差异

- `FUNCTION DUMP` 使用本版本私有格式（`RFNC` + CRC64），只保证本版本 RESTORE 往返，不兼容 Redis 7。
- `FUNCTION STATS` 是简化输出；`FUNCTION KILL` 在无脚本运行时返回 BUSY，不支持真正的脚本抢占。
- 没有 `FUNCTION LOAD_RO`，函数 flags 只实现 `no-writes` 对应的写命令拦截。
- AOF/RDB 使用函数源码回灌，不采用 Redis 7 的函数序列化格式或引擎多语言扩展。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 85 项测试通过，包含新增的 FUNCTION LOAD/FCALL 与 DUMP/RESTORE 测试，三节点集群组网与 `MOVED` 重定向，以及原有 RDB/AOF 重启回归。
