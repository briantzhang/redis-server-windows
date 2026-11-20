# Redis for Windows 5.0.14.1-bp67

本轮细化 ACL 选项级键权限与 COMMAND GETKEYSANDFLAGS：

- `SET key value GET` 现在按读写键处理，ACL 需要同时具备该键的读与写权限；普通 `SET` 仍只需要写权限。
- `COMMAND GETKEYSANDFLAGS` 对 SET 给出当前参数对应的有效键标志，不再只回文档里的 `variable_flags` 标记。
- 扩展 ACL DRYRUN 与 GETKEYSANDFLAGS 回归覆盖。
- 全量共 135 项，打包前使用当前构建重新跑通。

只提供 Windows x64 构建，未 push 到 tporadowski/redis。
