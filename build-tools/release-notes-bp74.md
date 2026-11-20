# 5.0.14.1-bp74

本版细化 ACL 选项级键权限。

- `GETEX` 的键权限现在按实际语义判定：总是要求读权限，带过期选项时同时要求写权限。
- `COMMAND GETKEYSANDFLAGS GETEX ...` 返回实际的 `RO`/`RW` 以及对应访问标记，不再使用静态命令级标志。
- `ACL DRYRUN` 与真实执行共用同一权限判定，读写模式测试覆盖 `%R~`、`%W~` 与 `~both:*`。
- Windows x64 Release 构建通过，140 项回归全部通过。

未 push，未发布。
