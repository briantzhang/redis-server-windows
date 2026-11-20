# Redis for Windows 5.0.14.1-bp81

本次在 bp80 基础上补齐 redis-cli 的 TLS 客户端能力：

- hiredis 阻塞连接新增 `redisConnectTLS()`，以 OpenSSL memory BIO 完成 TLS 握手、读写和关闭，兼容 Windows 的 RFD/IOCP 描述符模型。
- redis-cli 支持 `--tls`、`--sni`、`--cacert`、`--cert`、`--key`、`--insecure` 以及 `rediss://` URI；CA 校验、主机名校验、客户端证书和重连路径均使用相同配置。
- 新增黑盒回归 `test_redis_cli_tls_ping`，覆盖 CA 校验、环回地址主机名、`rediss://` 与错误 CA 拒绝。
- 更新 MSVC 工程的 OpenSSL 链接配置和打包/README 中的 TLS 客户端说明。

回归：142 项 Windows 原生测试全部通过。
