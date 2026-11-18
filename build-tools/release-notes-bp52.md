# Redis for Windows 5.0.14.1-bp52

本轮补齐集群运维查询命令，不改变集群协议或槽位分配算法：

- 新增 `CLUSTER MYSHARDID`：master 返回自己的节点 ID，replica 返回其 master 的 ID。
- `CLUSTER SHARDS` 每个分片新增 `shard-id` 字段，与 `MYSHARDID` 的语义一致。
- 新增 `CLUSTER MIGRATION`：列出本节点正在 importing / migrating 的槽位、方向和对端节点 ID。
- 新增 `CLUSTER SLOT-STATS [SLOTSRANGE start end ...]`：按范围返回本地 key 数和 owner 段数，省略范围时覆盖全部 16384 个槽。
- `CLUSTER HELP` 同步列出四个新条目。
- 回归新增 1 项，覆盖上述命令、参数校验和 HELP；全量共 122 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
