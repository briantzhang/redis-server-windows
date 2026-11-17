# Redis for Windows 5.0.14.1-bp27

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp26 之上加入 ACL 键读写权限。
`redis-server --version` 报告 `v=5.0.14.1-bp27`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp26 的改动

- `ACL SETUSER` 的键规则支持 `%R~pattern`（只读）、`%W~pattern`（只写）、`%RW~pattern`（读写，等同 `~pattern`）。
- `%R~*`、`%W~*` 可以按通配符授予全部键的单一方向权限；`~*`、`allkeys` 仍然是读写全开。
- 命令对每个键需要什么权限，取自该命令的键规格：`access` 需要读，`insert`/`update`/`delete` 需要写。
- `GETDEL`、`COPY` 的目标键同时需要读写；`BITOP` 的目标键需要写、源键需要读。
- `SET`、`SETEX` 的键规格随时间选项变化，按命令级写标志处理，因此 `%W~` 用户可以执行普通 `SET`。
- `ACL GETUSER` 的 `keys`、`ACL LIST`、`ACL SAVE` 输出保留 `%R~`/`%W~` 前缀，`ACL LOAD` 可以原样恢复。
- 选择器里的键规则同样生效，例如 `(+@write %W~log:*)`。

## 与 Redis 7 的差异

- `SET`、`SETEX` 不区分是否带 `GET` 选项，统一按命令级写标志判断。
- 没有 `%RW~` 之外的键规格扩展，也没有 `ACL GETUSER` 的 `keys` 之外的新字段。
- 命令键位置不固定、没有键规格的命令回退到命令级读写标志。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 90 项测试通过，新增：
- 只读、只写、读写三种模式下的 GET/SET/DEL 组合。
- `GETDEL`、`COPY`、`BITOP` 的读写双要求与目标键/源键区分。
- `ACL DRYRUN` 按同一规则预检，非法 `%X~`、`%~` 规则被拒绝。
- `ACL SAVE`/`ACL LOAD` 往返保留 `%R~`/`%W~` 规则。

未提交、未 push、未打包、未发布。
