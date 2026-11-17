# Redis for Windows 5.0.14.1-bp28

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp27 之上加入 TLS 主从复制。
`redis-server --version` 报告 `v=5.0.14.1-bp28`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp27 的改动

- 新增配置 `tls-replication yes|no`（默认 `no`），支持 `CONFIG GET/SET/REWRITE`。
- 从库按 `tls-replication yes` 连接主库的 `tls-port`：阻塞式 TLS 握手、`PING`/`REPLCONF`/`PSYNC`、RDB 接收和之后的增量命令都走同一个 TLS 会话。
- 主库把 `+FULLRESYNC`、`+CONTINUE`、RDB 长度与内容、传播心跳换行都通过加密通道发给 TLS 从库；明文从库仍走原来的原始 `write()`/IOCP 路径。
- RDB 由磁盘方式发送时，TLS 从库的分块写走 TLS 输出缓冲，密文未排空时依靠可写事件做背压，不读取下一块。
- 主库启用 `repl-diskless-sync yes` 且存在等待全量同步的 TLS 从库时，自动退回磁盘方式生成 RDB；磁盘无盘传输的子进程无法驱动 TLS 会话。
- 从库使用主库的可选客户端证书配置：主库要求客户端证书时，从库按 `tls-cert-file`/`tls-key-file` 出示，并用 `tls-ca-cert-file` 校验主库证书。
- 修复 Windows QFork 子进程里函数库 `static` 指针未继承导致 BGSAVE 崩溃的问题：子进程在恢复 `server` 结构后重新挂接函数库和索引。

## 与 Redis 6 的差异

- 只支持磁盘方式为 TLS 从库传 RDB，没有 TLS 版的无盘复制。
- 不支持 `tls-cluster`、证书热加载、`tls-protocols`、`tls-ciphers` 等配置。
- 复制链路不区分主库的 `tls-auth-clients optional` 行为，仍按本版既有的 `yes`/`no` 两档处理。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 91 项测试通过，新增：
- 主库开 `tls-port` 并启用客户端证书校验，从库以 `tls-replication yes` 完成全量同步。
- 200000 字节大值经 RDB 传输后完整可读，同步完成后的增量写入能在从库读到。
- 断开从库连接后重连走 `+CONTINUE` 部分重同步，`sync_partial_ok` 增加且重连后的写入继续传播。
- 明文复制基线不受影响。

未提交、未 push、未打包、未发布。
