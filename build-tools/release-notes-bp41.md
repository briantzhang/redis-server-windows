# Redis for Windows 5.0.14.1-bp41

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp40 之上加入 Redis 7.0 的
`sanitize-dump-payload`：`RESTORE` 在加载前完整校验 DUMP 内部编码，而不是只校验版本与 CRC64。
`redis-server --version` 报告 `v=5.0.14.1-bp41`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp40 的改动

- 新增 `sanitize-dump-payload no|yes|clients`，默认 `no`；`clients` 只校验客户端发来的 `RESTORE`，
  不校验从主库复制来的 payload。
- 校验器覆盖字符串（含 INT 与 LZF 编码）、list/set/zset/hash 的 ziplist、listpack、intset、zipmap 编码、
  quicklist 的每个 ziplist，以及流对象的 listpack 节点、last-id、消费组 PEL 与消费者。
- 校验失败返回 `Bad data format`，键不会被创建或覆盖；CRC 正确但结构损坏的 payload 也会被拒绝
  （例如首项 prevlen 非 0 或零节点的 quicklist、截断的哈希 payload）。
- 模块类型（`RDB_TYPE_MODULE`）直接拒绝：Windows 构建不加载模块，无法安全解析。
- 校验成本随 payload 大小线性增长，默认关闭；开销量级见 `OPS.md`。

## 与 Redis 7 的差异

- 校验器按本版实际支持的 RDB 类型实现，未覆盖 Redis 7 的新类型与模块 v2 的 opcode 集合。
- 只作用于 `RESTORE`；RDB 文件与 AOF 的加载路径不调用它。
- 与 7.0 相同，`clients` 模式下副本继续跟随可能已接受过该 payload 的主库。

## 验证

`--match sanitize_dump_payload` 通过：CRC64 参考向量；三种模式的默认值、非法值、`CONFIG SET/REWRITE`；
字符串、int/lzf 字符串、ziplist、quicklist、intset、哈希、skiplist、流与消费组的 DUMP/RESTORE 往返；
三种结构损坏但 CRC 正确的 payload 被拒绝且键不存在；`clients` 模式拒绝客户端 payload、接受合法 payload。

未提交、未 push、未打包、未发布。
