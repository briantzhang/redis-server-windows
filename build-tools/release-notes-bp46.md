# Redis for Windows 5.0.14.1-bp46

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版。bp46 让 `redis-check-aof.exe` 支持 bp45 引入的多段 AOF 目录布局。
`redis-server --version` 报告 `v=5.0.14.1-bp46`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp45 的改动

- `redis-check-aof.exe <appendonlydir>` 按 `<appendfilename>.manifest` 的顺序检查 base 与全部增量文件。
  单文件 AOF 的检查与修复方式不变。
- manifest 缺失、格式非法、base 顺序错误或没有增量文件都会直接报错，不会静默跳过。
- `--fix` 只截断最后一个增量文件的坏尾；base 与其他增量文件只读检查。
- 修正 README 与 BACKPORT-RELEASE.md 中滞后的回归数量（115 项）。

## 验证

AOF 相关 6 项回归通过；全量回归 `python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe`
共 115 项全部通过，日志末尾为 `OK`。新增用例覆盖多段目录的检查与修复、坏 manifest 的拒绝。

未提交、未 push、未打包、未发布。
