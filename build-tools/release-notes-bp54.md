# Redis for Windows 5.0.14.1-bp54

本轮补齐 `CLIENT INFO` / `CLIENT LIST` 的协议与客户端追踪可观测字段：

- 新增 `resp=<2|3>`，反映连接当前协商的 RESP 协议版本。
- 新增 `redir=<id>`，与 `CLIENT GETREDIR` 和 `CLIENT TRACKINGINFO` 的 redirect 保持一致；未启用追踪时为 `-1`。
- 回归新增 1 项，覆盖默认 RESP2 字段、追踪重定向、HELLO 3 后的 RESP3 字段；全量共 124 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
