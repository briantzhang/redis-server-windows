# Redis for Windows 5.0.14.1-bp33

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp32 之上把
`tls-auth-clients` 扩展成 `no` / `yes` / `optional` 三态。
`redis-server --version` 报告 `v=5.0.14.1-bp33`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp32 的改动

- `tls-auth-clients` 接受 `no`、`yes`、`optional`，配置文件、命令行、`CONFIG GET`、`CONFIG SET` 和 `CONFIG REWRITE` 都支持。
- `no`：不向客户端索取证书，保持原行为。
- `yes`：客户端必须出示由 `tls-ca-cert-file`/`tls-ca-cert-dir` 签发的证书，否则握手失败。
- `optional`：向客户端索取证书；出示了不受信证书的连接在握手中被拒绝，不带证书的匿名连接仍可继续使用。
- `yes` 和 `optional` 都要求配置 CA；缺少 CA 时启动或 `CONFIG SET` 会报
  `tls-auth-clients yes or optional requires tls-ca-cert-file or tls-ca-cert-dir`。
- `CONFIG SET tls-auth-clients` 与 bp29 的 TLS 配置项一样重建 SSL 上下文：新连接按新模式握手，已有连接不受影响；非法值返回错误并保留原值。
- `CONFIG REWRITE` 把三态值写成不带引号的 `tls-auth-clients <mode>`；值为默认的 `no` 时不额外写行。

## 与 Redis 6/7 的差异

- 默认值仍是 `no`（上游 Redis 默认 `yes`），与本回移版之前的行为保持一致，避免升级后旧客户端被拒。
- 证书只用于握手校验，不映射到 ACL 用户，也没有 `tls-client-cert-file` 之类按连接覆盖证书的配置。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 96 项测试通过，新增：
- `no` 下匿名 TLS 连接可用；非法值（`maybe`、`yes please`、`option`）被拒绝且模式不变。
- `optional` 下匿名连接和带 CA 签发证书的连接都可用，自签名客户端证书被拒绝。
- 运行时再切到 `yes` 后同一个匿名客户端被拒绝，带证书的连接仍可用。
- `CONFIG REWRITE` 后配置文件含 `tls-auth-clients optional`。
- 手工验证启动参数 `--tls-auth-clients optional`、配置文件读取、`CONFIG GET` 与 `CONFIG SET` 大小写不敏感，以及 `--tls-auth-clients maybe` 启动即报错。

未提交、未 push、未打包、未发布。
