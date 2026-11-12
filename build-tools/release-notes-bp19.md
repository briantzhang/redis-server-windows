# Redis for Windows 5.0.14.1-bp19

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp19`。

本版在 bp18 之上补上套接字状态的并发保护。没有新增命令。

## 相对 bp18 的改动

### 套接字状态加锁

bp18 让多个线程同时从完成端口取包，但每个套接字的 `iocpSockState` 仍被主线程和 IO 线程同时改写：主线程在投递读、写和接受时修改 `masks`、`wreqs` 和待写链表，IO 线程在完成时修改同一份数据。

每个 `iocpSockState` 现在带一把 `CRITICAL_SECTION`，状态创建时初始化，真正关闭时销毁。以下路径都在持锁时访问状态：

- 注册和取消读写事件
- 投递零长度读、重叠写和 `AcceptEx`
- 完成端口线程回收完成包
- 取出已接受的连接、标记 `CONNECT_PENDING`

锁在发起可能阻塞的调用前释放。`WSIOCP_SocketSend` 遇到尚未完成的连接时，先解锁再等待套接字可写。

## 不包含

RESP3、Functions、TLS、多段 AOF、新版 RDB、集群、Sentinel、客户端内存驱逐。命令解析与执行仍在主线程。没有引入 Redis 8 源码。只有 Windows x64。

## 验证

80 项黑盒回归在加锁后的构建上全部通过，使用独立临时目录和随机端口，不连接现有实例。
