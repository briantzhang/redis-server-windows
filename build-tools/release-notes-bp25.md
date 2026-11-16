# Redis for Windows 5.0.14.1-bp25

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp24 之上加入 TLS 客户端监听。
`redis-server --version` 报告 `v=5.0.14.1-bp25`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp24 的改动

- 新增 `tls-port`、`tls-cert-file`、`tls-key-file`、`tls-ca-cert-file`、`tls-ca-cert-dir`、`tls-auth-clients`。
- 用 OpenSSL 3 的 memory BIO 接入现有 Windows IOCP：密文仍由 `read()`/`write()` 收发，不把 `SSL_set_fd` 挂到完成端口上。
- 支持 TLS 1.2 和 TLS 1.3；明文 `port` 与 `tls-port` 可以同时监听。
- 配置 CA 后可用 `tls-auth-clients yes` 要求客户端证书，未提供或校验失败时握手被拒绝。
- `INFO server` 增加 `tls_port`，`CONFIG GET tls-*` 可读取当前配置。

## 与 Redis 6/7 的差异

- TLS 只覆盖客户端监听；主从复制和集群总线仍是明文，没有 `tls-replication`、`tls-cluster`。
- 没有 `tls-protocols`、`tls-ciphers`、`tls-prefer-server-ciphers` 等套件配置，最低协议固定为 TLS 1.2。
- 不支持证书热加载，改证书后需要重启。
- 运行时依赖同目录的 `libssl-3-x64.dll` 与 `libcrypto-3-x64.dll`。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 87 项测试通过，新增：
- 明文与 TLS 双监听、TLS 1.3 握手、256 KiB 二进制值往返、64 条流水线命令。
- `tls-auth-clients yes` 下无客户端证书被拒绝，带 CA 签发证书可完成 PING/SET/GET。
- 手工压测覆盖 4 MB 值、200 条流水线和 20 个并发 TLS 连接。

未提交、未 push、未打包、未发布。
