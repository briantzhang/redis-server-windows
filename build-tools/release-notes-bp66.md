# Redis for Windows 5.0.14.1-bp66

本轮修正文档一致性并增强 WAITAOF 的本机 fsync 等待语义：

- 更新 README.md 与 BACKPORT-RELEASE.md，移除关于 TLS 证书热加载和客户端内存分桶的过时说明。
- WAITAOF 在 `appendfsync always/no` 下仍同步 fsync；`appendfsync everysec` 下将专用 fsync 排入 BIO 后台线程并阻塞客户端，直到该次 fsync 完成或超时（超时返回 `[0, 0]`）。
- BIO 增加按作业类型累计的完成计数，每个 WAITAOF 客户端等待自己的目标计数。
- 新增回归覆盖 everysec 异步确认和零超时场景。
- 全量共 135 项，打包前使用当前构建重新跑通。

只提供 Windows x64 构建，未 push 到 tporadowski/redis。
