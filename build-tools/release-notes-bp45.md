# Redis for Windows 5.0.14.1-bp45

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版。bp45 把 AOF 升级为 Redis 7 的多段布局
（`appenddirname` + manifest + base/增量文件），并补上旧单文件 AOF 的自动迁移。
`redis-server --version` 报告 `v=5.0.14.1-bp45`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp44 的改动

- AOF 从单文件改为目录布局：`appenddirname`（默认 `appendonlydir`）下由
  `appendonly.aof.manifest` 记录一个 base 文件和一个增量文件，文件名形如
  `appendonly.aof.<seq>.base.aof|.base.rdb` 与 `appendonly.aof.<seq>.incr.aof`。
  `aof-use-rdb-preamble yes` 时 base 使用 RDB 前导，扩展名为 `.rdb`。
- 新增 `appenddirname` 配置项：只接受目录名，不能带路径分隔符；支持配置文件、
  `CONFIG GET` 和 `CONFIG REWRITE`，与 `appendfilename` 一样不支持 `CONFIG SET`。
  `CONFIG GET appendfilename` 也可以读取当前文件名。
- `BGREWRITEAOF` 完成时由父进程把重写缓冲区追加进新的 base 文件，创建新的增量文件，
  先写临时 manifest 再改名原子替换，之后删除旧 base 与旧增量文件；Windows 上先关闭旧
  文件句柄再删除。重写失败时回滚内存中的 manifest，保留原文件。
- 启动时按 manifest 顺序加载 base 与该清单列出的全部增量文件；空文件视为正常，
  尾部截断沿用 `aof-load-truncated`。没有 manifest 但存在旧的单文件 `appendonly.aof`
  时先按旧格式加载，再调度一次后台重写迁移到新布局，迁移成功后删除旧文件。
- AOF 关闭时执行 `BGREWRITEAOF` 同样生成多段 AOF（base + 增量 + manifest），
  不再写单文件；再次打开 AOF 时沿用该 manifest。
- `INFO persistence` 的 `aof_base_size` 报告最近一次重写生成的 base 文件大小。

## 与 Redis 7 的差异

- `redis-check-aof` 没有跟随多段布局，仍只接受单个 AOF 文件；需要检查时对具体的
  `.base.aof` 或 `.incr.aof` 执行，manifest 不参与检查。
- manifest 同一时刻只包含一个增量文件；加载支持清单里出现多个增量文件，但没有实现
  重写期间按大小自动切换增量文件的分段策略。
- `appenddirname` 不支持运行时修改；修改目录或文件名需要停机并迁移数据。
- 没有引入 Redis 8 的源码，AOF 逻辑按 Redis 7 的布局在 5.0 的 AOF 代码上手工适配。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe`
共 113 项全部通过，日志末尾为 `OK`。新增用例覆盖 manifest 创建与重启加载、
`BGREWRITEAOF` 的序号轮转与旧文件清理、旧单文件迁移、增量文件尾部截断恢复；
原有的 AOF 时间戳、运行时开关与重启用例同步改为读取多段文件。

未提交、未 push、未打包、未发布。
