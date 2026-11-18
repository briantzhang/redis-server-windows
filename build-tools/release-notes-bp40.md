# Redis for Windows 5.0.14.1-bp40

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp39 之上加入 Redis 7.0 的
`repl-diskless-load`：从库可以不再把无盘 RDB 先落盘，而是直接解析。
`redis-server --version` 报告 `v=5.0.14.1-bp40`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp39 的改动

- `repl-diskless-load disabled|on-empty-db|swapdb`，默认 `disabled`（沿用底座行为：先写临时文件再加载）。
- `on-empty-db`：清空当前数据集后直接解析流式 RDB；本地已有数据时拒绝进入该模式。
- `swapdb`：把 RDB 装入一组临时数据库，全部加载成功后才整体替换当前数据集；加载失败时旧数据保持不变。
- 直接解析期间的内存会暂时接近两份数据，`OPS.md` 已注明；`INFO persistence` 的加载状态与普通加载一致。
- 运行时可用 `CONFIG SET` 切换，`CONFIG REWRITE` 写回配置文件；非法值报错并保留原值。

## 与 Redis 7 的差异

- 只支持这三种模式；没有按需二次校验等附加开关。
- 解析器仍是本版 RDB 加载器，模块类型等 Redis 7 的数据形态不在范围内。
- `sanitize-dump-payload` 不参与复制加载路径，复制数据只校验 RDB 版本与 CRC。

## 验证

`--match repl_diskless_load` 通过：`swapdb` 在已有数据上加载成功并替换旧数据，加载失败时旧键仍在；
`on-empty-db` 在空库上成功、在非空库上拒绝；`disabled` 仍写临时文件；三种模式都可以 `CONFIG SET`、
`CONFIG REWRITE` 并在重启后保持。

未提交、未 push、未打包、未发布。
