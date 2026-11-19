# Redis for Windows 5.0.14.1-bp60

本轮补齐 Streams 高级命令：

- 新增 `XACKDEL key group ID [ID ...] [KEEPREF]`：返回 `[acked, deleted]`，在确认 PEL 条目的同时删除对应流条目。
- `KEEPREF` 只确认不移除 PEL 引用；不存在键/组返回 `[0,0]`；非法 ID 和错误类型返回错误。
- 现有 `XAUTOCLAIM` 的 COUNT/JUSTID/独占 start、错误参数与空 PEL 回归保持不变。
- 全量共 129 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
