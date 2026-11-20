# 5.0.14.1-bp73

本版为 WAITAOF 增加副本确认语义。

- 副本通过 `REPLCONF ACK ... FACK <offset>` 汇报已 fsync 的复制偏移；主库据此统计 `numreplicas`。
- `WAITAOF` 同时等待本地 fsync 与副本 FACK，超时回复返回实际获得的本地和副本确认数。
- `appendfsync everysec` 的后台 fsync 在 BIO 完成后发布可确认偏移；`always` 在同步 fsync 后发布。
- 没有连接副本时，请求的副本确认数按 0 处理，保持单机可直接返回。
- 新增三节点（主库 + 副本）回归，覆盖 everysec 下多次写入的 WAITAOF 副本确认。
- Windows x64 Release 构建通过，140 项回归全部通过。

未 push，未发布。
