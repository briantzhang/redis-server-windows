# Redis for Windows 5.0.14.1-bp22

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp22`。

本版在 bp21 之上补齐三处元数据：命令文档、键标志和 ACL 选择器。

## 相对 bp21 的改动

### COMMAND DOCS

- 回复新增 `complexity` 和 `history`，`arguments` 从一整串字符串改为按层级展开的参数数组，每个参数带 `name`、`type`、`token`、`since` 和 `flags`。
- 数据取自 Redis 7.0.15 的命令表，覆盖本版实际存在的 236 条命令。`ZRANK` 和 `ZREVRANK` 的 `WITHSCORE` 参数取自 Redis 7.2.5，与本版实现一致。
- 没有文档的命令（`LOLWUT`、`WAITAOF`、内部握手命令）仍只回 `group=unknown`。

### COMMAND GETKEYSANDFLAGS

- 键标志改为按每条命令的键规格给出，不再整条命令一个标志。`BITOP AND dest src` 的 `dest` 是 `OW update`，`src` 是 `RO access`。
- 键位置不固定的参数仍按命令级标志近似，例如 `SORT` 的 `STORE` 键。

### ACL 选择器

- `ACL SETUSER` 接受 `(...)` 选择器。一个用户有一套根规则，加上任意个选择器；根规则或任一选择器放行，命令就被允许。
- `ACL GETUSER` 新增 `selectors` 字段，`ACL LIST` 与 `ACL SAVE` 也会写出选择器。`clearselectors` 清掉追加的选择器，保留根规则。
- 没有按键的读写权限位：一个选择器对匹配到的键一律放行，不区分读和写。

## 不包含

RESP3、Functions、TLS、多段 AOF、新版 RDB、集群、Sentinel。命令解析与执行仍在主线程。没有引入 Redis 8 源码。只有 Windows x64。

## 验证

81 项黑盒回归全部通过。新增的用例覆盖选择器的放行与拒绝、`BITOP` 的分键标志，以及 `COMMAND DOCS` 的参数层级与 `ZRANK` 的历史记录。测试使用独立临时目录和随机端口，不连接现有实例。
