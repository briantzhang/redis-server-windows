# Redis for Windows 5.0.14.1-bp43

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp42 之上做一轮 Windows 底座审计：
补齐 QFork 子进程的持久化路径，并把此前靠手工验证的平台行为写成回归测试。
`redis-server --version` 报告 `v=5.0.14.1-bp43`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp42 的改动

- 无盘全量同步的子进程改用 `rdbSaveRioWithEOFMark(rio, NULL, rdbSaveInfo*)` 与
  `rdbPopulateSaveInfo()`；底座原来的两参数原型与现实现不匹配，会让无盘同步在子进程里出错。
- 为 TLS/集群套接字补上 `WSIOCP_SocketAttach` 声明，子进程复制出的套接字能正常挂到 IOCP。
- 每次无盘同步前释放上一轮为子进程保存的 `WSAPROTOCOL_INFO` 数组；`WSADuplicateSocket` 失败时让
  `BeginForkOperation` 失败并回收已分配内存，而不是把未初始化数据交给子进程。
- 新增 Windows 平台回归：持续写入下的 `BGSAVE`/`BGREWRITEAOF`（AOF 开启）、运行时
  `CONFIG SET appendonly yes/no`、AOF 关闭后仍能完成重写且不残留临时文件、`SAVE` 后重启、
  3 字节分段发送的命令、1 MiB value 分 4 KiB 写入、1500 条命令的流水线、150 次连接抖动
  （半条命令、不读回复、丢弃流水线）后 `connected_clients` 回落、空闲超时经 IOCP 断开。

## 与上游 Windows 移植的差异

- Windows 不加载模块，含模块数据的 RDB/DUMP 不在覆盖范围内；Functions 走 Lua，不受影响。
- QFork 仍是“复制父进程内存再执行”的模拟，不是真正的写时复制；大实例的 fork 延迟与内存峰值高于 Linux。

## 验证

`--match windows_aof_bgsave`、`--match windows_socket_churn`、`--match windows_persistence_toggles`、
`--match repl_diskless_load`、`--match tls_replication_diskless` 全部通过；全量回归 110 项 OK。

未提交、未 push、未打包、未发布。
