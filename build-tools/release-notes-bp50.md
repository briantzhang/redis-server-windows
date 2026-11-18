# Redis for Windows 5.0.14.1-bp50

本轮将客户端内存记账对齐到 Redis 7 的分桶思路，但不引入新版连接层或复制缓冲改动：

- `querybuf`、`argv`、固定输出缓冲、动态回复缓冲和客户端对象分别计入 `maxmemory-clients` 估算。
- 大 RESP 参数会从查询缓冲转入 `argv`，新增 `mid_querybuf_alloc` 缓存该分配，避免误判为释放，同时由 `mid_argv` 记录解析后的参数数据。
- `CLIENT INFO` / `CLIENT LIST` 新增 `qbuf-mem`、`argv-mem` 和 `tot-mem` 字段，便于定位内存占用最高的客户端。
- 参数数组被重写或新一轮命令开始解析时刷新分桶，命令执行完成后不保留过期的高水位。
- 回归新增 1 项，覆盖分桶字段、暂停客户端的大参数查询缓冲与阻塞客户端的参数数组；全量共 120 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
