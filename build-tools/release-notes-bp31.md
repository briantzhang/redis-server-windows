# Redis for Windows 5.0.14.1-bp31

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp30 之上补两类 keyspace 通知。
`redis-server --version` 报告 `v=5.0.14.1-bp31`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp30 的改动

- `notify-keyspace-events` 新增 `n`：任何命令创建新键时发 `new` 事件，来源是 `dbAdd()`，因此 `SET`、`HSET`、`LPUSH`、`RESTORE` 等只要真正新增键都会触发。
- 新增 `m`：普通读路径查询不到键时发 `keymiss` 事件，覆盖 `GET`、`EXISTS`、`HGET` 等使用读查找的命令；键存在时不发。
- `keymiss` 在 RDB/AOF 子进程运行期间不产生，避免 fork 出来的子进程发出无意义的通知。
- `A` 别名不包含 `n` 和 `m`，与 Redis 7.0 一致；`CONFIG GET notify-keyspace-events` 会分别列出 `n`、`m` 与其它类型字符。
- keyspace 和 keyevent 两种频道形式都按原有规则生成：`__keyspace@<db>__:<key>` 携带事件名，`__keyevent@<db>__:<event>` 携带键名。

## 与 Redis 7 的差异

- 没有实现 `d`（模块键类型）事件：本版没有 `RedisModule_NotifyKeyspaceEvent` 接口，模块无法产生这类事件。
- `keymiss` 不区分 `LOOKUP_NOSTATS` 等后续版本引入的内部查找标志。
- `new` 事件的发出位置固定在 `dbAdd()`，与 Redis 7 的时机一致，但没有额外的命令级开关。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 94 项测试通过，新增：
- `KEnm` 下 `SET` 新键收到 `__keyevent@0__:new`，读不存在的键收到 `__keyevent@0__:keymiss`。
- 读已存在的键不发 `keymiss`。
- `KEA` 的 `CONFIG GET` 输出为 `AKE`，不含 `n`/`m`；`KEnm` 输出包含这两类。

未提交、未 push、未打包、未发布。
