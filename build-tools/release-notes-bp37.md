# Redis for Windows 5.0.14.1-bp37

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp36 之上让集群总线也支持 TLS：
`tls-cluster yes` 时节点之间的握手、PING/PONG、槽位广播和故障转移投票都走加密连接。
`redis-server --version` 报告 `v=5.0.14.1-bp37`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp36 的改动

- 新增 `tls-cluster`（默认 `no`，仅 `cluster-enabled yes` 时有意义），启动时把总线连接接入与客户端监听相同的
  OpenSSL 收发路径；客户端端口与总线端口的明文/TLS 选择互不影响。
- 总线双方在握手阶段互相校验证书链，要求所有节点配置同一 `tls-ca-cert-file`/`tls-ca-cert-dir`；
  一侧开启、一侧关闭时握手失败，节点互相不可见，`CLUSTER INFO` 的 `cluster_state` 不会变成 `ok`。
- 总线 TLS 沿用 `tls-protocols`/`tls-ciphers`/`tls-ciphersuites` 的协议与套件选择。
- `redis.conf` 增加 `tls-cluster` 说明，Windows 发布包说明同步更新。

## 与 Redis 6 的差异

- 证书只在进程启动时读取，没有热加载：换证书或开关 `tls-cluster` 需要重启进程。
- `tls-cluster` 不影响客户端连接；集群模式下的客户端监听仍由 `tls-port` 单独控制。
- 总线仍使用底座 Redis 5.0 的报文格式与集群状态机。

## 验证

`--match cluster_tls` 通过：三个节点全部开启 `tls-cluster` 后可以 `CLUSTER MEET`、分配槽位并完成
`MOVED` 重定向，`CLUSTER LINKS` 能看到加密链路；一侧关闭 `tls-cluster` 的节点无法加入集群；
证书不受信的节点握手被拒绝。

未提交、未 push、未打包、未发布。
