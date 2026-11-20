# 5.0.14.1-bp72

本版细化流消费者活跃时间语义。

- `XINFO CONSUMERS` 的 `inactive` 在 `XCLAIM` 与 `XAUTOCLAIM` 成功投递后刷新，包含 `JUSTID` 回复路径，避免“已重新投递但仍显示长期不活跃”的误导。
- `XINFO STREAM ... FULL` 中消费者 `active-time` 与 `inactive` 使用同一时钟；PEL 中较新的投递时间会同步到消费者活跃时间。
- 新增回归覆盖 XCLAIM JUSTID、XAUTOCLAIM、PEL 与消费者计数的一致性。
- Windows x64 Release 构建通过，139 项回归全部通过。

未 push，未发布。
