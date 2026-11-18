# Redis for Windows 5.0.14.1-bp32

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp31 之上加入 AOF 时间戳注释。
`redis-server --version` 报告 `v=5.0.14.1-bp32`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp31 的改动

- 新增配置 `aof-timestamp-enabled yes|no`（默认 `no`），支持配置文件、`CONFIG GET`、`CONFIG SET` 和 `CONFIG REWRITE`。
- 打开后，AOF 写入前最多每秒追加一行 `#TS:<unix 时间戳>`；同一秒内的多条命令共用一条注释，不会为每条命令重复写。
- `BGREWRITEAOF` 生成的新文件在开头记录一次写入时间（即使这一秒已经写过），方便判断重写起点。
- 加载 AOF 时跳过以 `#` 开头的注释行，包括带 RDB 前置的数据文件；旧版本 AOF 和关闭该选项的 AOF 完全不受影响。
- 注释由写入路径直接拼进 AOF 缓冲区，不经过命令重放，因此不改变复制流和 AOF 偏移以外的语义。

## 与 Redis 7 的差异

- 只支持 `#TS:` 注释，没有多段 AOF manifest 中的时间字段。
- 注释按“每秒最多一条”生成，不做更细的时间戳对齐。
- `INFO persistence` 没有新增时间戳相关字段。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 95 项测试通过，新增：
- 打开选项后 `SET` 立即在 AOF 中产生 `#TS:`，时间与当前时间相差不超过 120 秒。
- `BGREWRITEAOF` 后新文件以 `#TS:` 开头。
- 带注释的 AOF 重启后正常加载，键值完整。
- 关闭选项后新写入不再追加注释。

未提交、未 push、未打包、未发布。
