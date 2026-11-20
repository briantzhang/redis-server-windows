# 5.0.14.1-bp76

本版补齐 COMMAND 元数据。

- `COMMAND DOCS` 新增 `key_specs` 字段：有键规格的命令返回 1，便于客户端区分键型命令与无键命令。
- 保持原有 `summary`、`complexity`、`since`、`group`、`arguments`、`history` 的 RESP2/RESP3 编码不变。
- Windows x64 Release 构建通过，定向 COMMAND/OBJECT 回归与全量回归通过。

未 push，未发布。
