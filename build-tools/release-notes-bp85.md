# Redis for Windows 5.0.14.1-bp85

本次补强 WAITAOF 与集群可观测性回归：

- WAITAOF 增加 numlocal 越界、负数超时和零超时边界验证。
- CLUSTER LINKS 测试断言每个链路节点 ID 均来自 CLUSTER NODES，防止报告未知握手节点。
- 保留 CLUSTER SHARDS 与 SLOTS 的槽位、RESP2/RESP3 字段覆盖。

全量 Windows 原生回归在提交前执行。
