# Redis for Windows 5.0.14.1-bp51

本轮补齐 TLS 证书路径的运行期热加载：

- `CONFIG SET` 现在可以修改 `tls-cert-file`、`tls-key-file`、`tls-ca-cert-file` 和 `tls-ca-cert-dir`，并立即重建服务端与客户端 TLS context。
- 新连接使用新的证书、私钥和 CA；已有连接继续使用原有 SSL 对象，不受热加载影响。
- 新路径无法加载时返回错误并回滚到原配置，避免把失败的路径写入运行中的服务器状态。
- 回归新增 1 项，覆盖新连接生效、旧连接保持、失败回滚与 `CONFIG GET` 一致性；全量共 121 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
