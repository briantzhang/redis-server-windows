# Redis for Windows 5.0.14.1-bp36

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp35 之上加入 Redis 7.0 的
`CLUSTER SHARDS` 与 `CLUSTER LINKS`，把集群状态从“逐个节点一个数组”改成“按分片汇总”的视图。
`redis-server --version` 报告 `v=5.0.14.1-bp36`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp35 的改动

- 新增 `CLUSTER SHARDS`：每个持有槽位的主节点返回一条分片记录，字段 `slots` 是 `start end` 对组成的
  区间数组（稀疏槽位会给出多段），`nodes` 是该主节点加上健康、可公告的副本；故障节点与
  `CLUSTER NODE NOANNOUNCE` 的副本不列出。
- 每个节点条目包含 `id`、`port`、`ip`、`endpoint`、`role`、`replication-offset`、`health`；
  地址按 `cluster-preferred-endpoint` 的规则给出。
- 新增 `CLUSTER LINKS`：按总线连接方向列出 `direction`（`from`/`to`）、`node`、`create-time`、
  `events`、`send-buffer-allocated` 与 `send-buffer-used`，入站链路的发送缓冲用于回 `PONG`。
- 两条命令在 RESP2 下把 map 展开成扁平数组，RESP3 下返回真正的 map，和本版其它 map 回复一致。

## 与 Redis 7 的差异

- `CLUSTER SHARDS` 只统计持有槽位的主节点；没有槽位的主节点不产生分片记录。
- 没有 Redis 7.2 的 slot 级 `migrating`/`importing` 字段和额外节点属性。
- 集群本身仍是底座 Redis 5.0 的实现，槽迁移、故障转移和总线报文格式未改。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe --match cluster_shards` 通过：
单节点分配槽位后能看到分片、槽区间和自身节点条目，`CLUSTER LINKS` 在无对端时返回空数组；
多节点组网后每条分片只带自己的槽位，副本进入 `nodes`，故障标记的副本被过滤。

未提交、未 push、未打包、未发布。
