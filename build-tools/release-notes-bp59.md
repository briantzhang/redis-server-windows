# Redis for Windows 5.0.14.1-bp59

本轮补齐 `GETDEL` / `GETEX` 的事务和重启传播回归：

- 验证 `MULTI` 中 `GETEX PX` 与 `GETDEL` 的排队与 EXEC 返回值。
- 验证二进制值 `GETDEL` 后键被删除。
- 验证重启后 GETEX 设置的 TTL 保留、GETDEL 删除的键保持不存在。
- 全量共 128 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
