# Redis for Windows 5.0.14.1-bp63

本轮补齐 Windows 构建下 `redis-cli --cluster` 的完整性回归：

- 验证 cluster manager 帮助中列出 create、check、info、fix、reshard、rebalance、add-node、del-node、call、set-timeout、import、help。
- 验证 replicas、yes、replace、simulate 等主要选项都出现在帮助中。
- 全量共 132 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
