# Redis for Windows 5.0.14.1-bp48

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版。bp48 补上 `STRALGO LCS` 兼容别名。
`redis-server --version` 报告 `v=5.0.14.1-bp48`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp47 的改动

- 新增 `STRALGO LCS key1 key2 [LEN|IDX ...]`：作为现有 `LCS` 命令的 Redis 6.0 兼容别名，
  复用同一套算法和回复格式；非 `LCS` 子命令返回语法错误。
- 新增回归用例覆盖字符串、LEN、IDX+WITHMATCHLEN、非法子命令和冲突选项。

## 验证

`STRALGO` 与 `LCS` 2 项回归通过；全量回归共 118 项，日志末尾为 `OK`。
未提交、未 push、未打包、未发布。
