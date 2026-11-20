# 5.0.14.1-bp75

本版补强集群只读可观测性。

- `CLUSTER SHARDS [shard-id]` 支持按分片 ID 精确筛选，便于脚本只检查一个分片。
- `CLUSTER HELP` 同步更新参数说明；无匹配分片返回空数组，参数过多按既有语法错误处理。
- RESP2/RESP3 的 map 编码保持不变，现有 `SLOTS`、`LINKS`、`SLOT-STATS` 与槽键查询行为不变。
- Windows x64 Release 构建通过，140 项回归全部通过。

未 push，未发布。
