# Redis for Windows 5.0.14.1-bp38

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp37 之上加入 Redis 7.0 的节点公告选项：
`cluster-announce-hostname`、`cluster-preferred-endpoint` 与 `replica-announced`。
`redis-server --version` 报告 `v=5.0.14.1-bp38`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp37 的改动

- `cluster-announce-hostname <name>`：对外公告的主机名，空字符串表示不公告。
- `cluster-preferred-endpoint ip|hostname`：决定 `CLUSTER SLOTS`、`CLUSTER SHARDS`、`CLUSTER NODES`
  以及重定向回复里使用 IP 还是主机名；主机名为空时回落到 IP。
- `replica-announced yes|no`：副本是否出现在 `CLUSTER SLOTS`/`SHARDS` 的节点列表和 `CLUSTER NODES`
  的对外视图中；`no` 时副本仍参与复制和故障转移投票。
- 三项都可以 `CONFIG SET` 运行时修改，`CONFIG REWRITE` 会写回配置文件；非法枚举值报错并保留原值。

## 与 Redis 7 的差异

- 主机名只做字符串公告，不做 DNS 校验或反向解析。
- 没有 `cluster-announce-human-nodename` 与 `CLUSTER MYSHARDID` 等 7.2 的附加字段。
- 总线报文格式仍是 Redis 5.0，组网需要所有节点都使用本版。

## 验证

`--match cluster_announce` 与 `--match ops_config` 通过：设置主机名后 `CLUSTER SLOTS`/`SHARDS`
返回主机名端点，切回 `ip` 后恢复地址；`replica-announced no` 让副本从 `CLUSTER SLOTS` 消失
但复制链路仍为 `up`；非法端点值被拒绝，`CONFIG REWRITE` 后重启保持新值。

未提交、未 push、未打包、未发布。
