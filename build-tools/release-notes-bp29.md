# Redis for Windows 5.0.14.1-bp29

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp28 之上补全 TLS 配置项。
`redis-server --version` 报告 `v=5.0.14.1-bp29`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp28 的改动

- 新增 `tls-protocols`：空格分隔的 `TLSv1`/`TLSv1.1`/`TLSv1.2`/`TLSv1.3` 列表，换算成上下文的最低和最高协议版本；留空沿用本版 TLS 1.2 下限。
- 新增 `tls-ciphers`：TLS 1.2 及以下的套件列表，交给 `SSL_CTX_set_cipher_list` 校验和安装。
- 新增 `tls-ciphersuites`：TLS 1.3 的套件列表，交给 `SSL_CTX_set_ciphersuites`。
- 新增 `tls-prefer-server-ciphers yes|no`：开启 `SSL_OP_CIPHER_SERVER_PREFERENCE`，由服务端决定套件顺序。
- 新增 `tls-session-caching yes|no`：控制 `SSL_SESS_CACHE_SERVER`/`SSL_SESS_CACHE_CLIENT`，关闭后不复用 TLS 会话。
- 五项都支持配置文件、命令行参数、`CONFIG GET`、`CONFIG SET` 和 `CONFIG REWRITE`。
- `CONFIG SET` 会重建服务端 SSL 上下文并丢弃缓存的客户端上下文：新建连接用新设置，已有连接持有旧上下文继续工作；值非法（未知协议名、空套件列表、非 yes/no）时返回错误并保留原设置。
- 监听和 `tls-replication` 的客户端上下文共用同一组设置，副本连主库时也按 `tls-protocols`/`tls-ciphers*` 协商。
- TLS 未监听（`tls-port 0`）时，`CONFIG SET` 仍会在临时上下文上校验取值，避免把错误配置留到下次重启才暴露。

## 与 Redis 6 的差异

- `tls-protocols` 留空时的下限固定为 TLS 1.2，不跟随 OpenSSL 的发行版默认值。
- 没有 `tls-cluster`、证书热加载、`tls-key-file-pass` 等配置。
- 重建上下文时不会重新加载证书路径之外的内容；修改 `tls-cert-file`/`tls-key-file` 仍需重启。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 92 项测试通过，新增：
- 五个配置项的默认值、合法值与非法值（未知协议、错误套件、非 yes/no）校验，非法值不改变原设置。
- 运行时应用后旧连接继续可用、新连接按新协议握手；只允许 TLS 1.3 时 TLS 1.2 客户端被拒绝。
- `CONFIG REWRITE` 把 `tls-protocols`、`tls-session-caching` 等当前值写入配置文件。
- 命令行启动参数 `--tls-protocols TLSv1.3` 生效。

未提交、未 push、未打包、未发布。
