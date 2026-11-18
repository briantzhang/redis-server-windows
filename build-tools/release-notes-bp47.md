# Redis for Windows 5.0.14.1-bp47

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版。bp47 对齐 WAITAOF 与 bp45 引入的多段 AOF 布局。
`redis-server --version` 报告 `v=5.0.14.1-bp47`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp46 的改动

- WAITAOF 在同步 fsync 后更新 `aof_fsync_offset`，包含重写切换后的新增量文件、`everysec` 模式和旧单文件布局。
- 增加 fd 有效性检查：`appendonly yes` 但文件句柄尚未打开时返回错误，而不是用无效句柄调用 fsync。
- 新增两项回归用例：重写前后的 WAITAOF 确认与重启加载、`appendfsync everysec` 下的强制同步。

## 验证

WAITAOF/AOF 相关 9 项回归通过；全量回归共 117 项，日志末尾为 `OK`。
未提交、未 push、未打包、未发布。
