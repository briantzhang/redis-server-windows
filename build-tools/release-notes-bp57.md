# Redis for Windows 5.0.14.1-bp57

本轮补齐绝对过期时间查询的回归：

- 覆盖 `EXPIRETIME` / `PEXPIRETIME` 对不存在、无过期、毫秒精度和秒四舍五入的返回语义。
- 验证查询不会刷新键的 LRU 访问时间。
- 验证 `DUMP` + `RESTORE ... ABSTTL` 往返后绝对截止时间保持不变。
- 保留 WATCH/MULTI 下不触发 dirty、参数和类型错误路径的已有回归；全量共 126 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
