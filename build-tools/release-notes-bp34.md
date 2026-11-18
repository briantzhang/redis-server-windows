# Redis for Windows 5.0.14.1-bp34

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp33 之上让 TLS 从库也能使用
`repl-diskless-sync` 无盘全量同步，并修复了两处导致这条链路不可用的问题。
`redis-server --version` 报告 `v=5.0.14.1-bp34`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp33 的改动

- 主库启用 `repl-diskless-sync yes` 时，TLS 从库不再退回磁盘方式，RDB 不落盘。
- 实现方式：RDB 子进程没有 TLS 状态，无法直接写 TLS 套接字。父进程在 fork 前为每个 TLS 从库创建一对回环套接字，子进程把 RDB 写进其中一端，父进程读另一端并经过从库的 TLS 会话转发；回环套接字 EOF 即表示载荷结束。父进程读端注册到 IOCP 才能收到可读事件。
- 转发带背压：父进程每次最多读 16 KiB，`tlsWrite()` 报告密文仍在排队时等套接字可写再继续，不会把整个 RDB 堆进内存。
- TLS 从库与明文从库可以同时连同一个无盘主库：明文从库仍由子进程直接写套接字，TLS 从库走转发路径。
- 磁盘方式的 TLS 全量同步、增量传播和部分重同步行为不变。

## 修复的底座问题

- `Win32_QFork_impl.c` 里 RDB 子进程调用的是两参数的 `rdbSaveRioWithEOFMark(&slave_sockets,NULL)`，而 `rdb.c` 的实现接收三个参数（多了 `rdbSaveInfo *rsi`）。缺失参数取到随机栈内容，子进程写第一个 RDB 辅助字段时崩溃，这个底座上任何 `repl-diskless-sync yes` 的全量同步（明文或 TLS）都无法完成。现在子进程按 fork 的做法调用 `rdbPopulateSaveInfo()` 并传入 `rsi`。
- 从库的 `readSyncBulkPayload()` 在 TLS 下每消费 4 KiB 就递归调用一次自己以读取 OpenSSL 已缓冲的明文。大 RDB 会把递归深度推到上千层，20 MB 的同步直接 `EXCEPTION_STACK_OVERFLOW`。现在改为循环消费，不再占用栈；这个问题同样影响磁盘方式的 TLS 全量同步。

## 与 Redis 6/7 的差异

- 回环套接字是 Windows 上的实现手段（Unix 用匿名管道，因为 Windows 的 IOCP 事件循环只处理套接字）。每个 TLS 从库在同步期间多一对本地连接，同步结束即关闭。
- RDB 子进程结束后，父进程转发完成才把从库置为可发送增量，仍沿用 5.0 的 `REPLCONF ACK` 流程。
- 没有实现 7.0 的多从库共享单条管道广播，每个 TLS 从库各有一条转发链路。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 97 项测试通过，新增：
- 无盘主库同时服务一个 TLS 从库和一个明文从库：两个从库都完成全量同步，8 MB 不可压缩的值和多个键一致。
- 同步后主库目录没有生成 `dump.rdb`，确认走的是无盘路径。
- 全量同步完成后，主库的新写入仍能正常传播到 TLS 从库。
- 手工用 20 MB 值复现并验证：修复前从库栈溢出崩溃，修复后 0.7 秒内同步完成且数据一致，主库仍无 `dump.rdb`。
- 全量回归同时覆盖了修复后的明文无盘复制路径，以及原有的磁盘方式 TLS 复制用例。

未提交、未 push、未打包、未发布。
