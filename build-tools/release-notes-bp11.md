# Redis for Windows 5.0.14.1-bp11

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp11`。

本版在 bp10 的基础上补齐命令文档查询、有序集合排名附带分数，以及 ACL 帮助文本。

## 相对 bp10 新增

### COMMAND DOCS

- `COMMAND DOCS [command ...]`：返回命令文档。无参数时覆盖全部已注册命令；给出名字时只返回找到的命令，未知名字被跳过。
- 每个命令的文档是 RESP2 扁平字段数组，常见字段为 `summary`、`since`、`group`、`arguments`。内容来自 `help.h`；没有帮助条目的命令仍会出现，此时 `group=unknown`，且可能没有 summary/arguments。
- 本版不输出上游 Redis 7 的完整参数树、`complexity`、`history` 或子命令嵌套文档。
- `COMMAND HELP` 现已列出 `DOCS`。

### ZRANK / ZREVRANK WITHSCORE

- `ZRANK key member [WITHSCORE]`、`ZREVRANK key member [WITHSCORE]`：在排名后附带该成员分数（bulk string，与 `ZSCORE` 相同格式）。
- 成员不存在或键不存在时：不带选项返回 nil bulk；带 `WITHSCORE` 返回 null array（`*-1`）。
- 错误拼写（如 `WITHSCORES`）或多余参数报 syntax error；错误类型仍返回 WRONGTYPE。

### ACL HELP / WHOAMI

- `ACL HELP` 列出已实现的 ACL 子命令；与 `ACL WHOAMI` 一样，对任何已认证用户放行，不要求持有 `acl` 命令位。
- `ACL WHOAMI` 本就已实现，回归用例补上返回 `default` 与非法参数检查。

## bp10 已有的能力

延迟直方图与 `INFO latencystats`、ACL 频道权限、脚本内 ACL、`SCRIPT FLUSH ASYNC|SYNC`；bp9 的 `ACL LOG`/`SAVE`/`LOAD`、`CLIENT SETINFO`、分片 Pub/Sub、`INFO errorstats`；以及更早各轮的 GEOSEARCH、流命令、多键弹出、COPY、条件过期等。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐。ACL 不含 Redis 7 的选择器与按键读写权限位。`COMMAND DOCS` 不是上游完整文档图。命令语义参照 Redis `7.0.15`、`7.2.5` 与 `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

69 项黑盒回归全部通过，其中本轮覆盖：`COMMAND DOCS` 与 `COMMAND HELP`；`ZRANK`/`ZREVRANK WITHSCORE` 的命中、缺失、错误类型与非法参数；`ACL HELP`/`WHOAMI`。测试使用独立临时目录和随机端口，不连接现有实例。
