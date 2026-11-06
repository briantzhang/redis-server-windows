# Redis for Windows 5.0.14.1-bp1

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.0 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp1`。

## 本次新增功能

### 有序集合运算

| 命令 | 说明 |
| --- | --- |
| `ZUNION numkeys key [key ...] [WEIGHTS w ...] [AGGREGATE SUM\|MIN\|MAX] [WITHSCORES]` | 多个有序集合的并集，只返回结果，不写入数据库 |
| `ZINTER numkeys key [key ...] [WEIGHTS w ...] [AGGREGATE SUM\|MIN\|MAX] [WITHSCORES]` | 多个有序集合的交集 |
| `ZDIFF numkeys key [key ...] [WITHSCORES]` | 第一个键减去其余键的差集 |
| `ZDIFFSTORE destination numkeys key [key ...]` | 计算差集并写入目标键 |

### 键与连接

- `COPY source destination [DB db] [REPLACE]`：深拷贝字符串、列表、集合、有序集合、哈希和流。目标键拥有独立存储和独立 TTL；流的消费组与待确认消息一并复制；不带 `REPLACE` 时不覆盖已存在的目标。
- `RESET`：把当前连接切回 0 号库并清除客户端状态（连接名等），不影响其他连接和数据库数据。

### ACL（Redis 6 范围）

- `ACL SETUSER`、`ACL DELUSER`、`ACL LIST`、`ACL WHOAMI`，以及 `AUTH username password`。
- 每个用户一套权限：`on/off`、`>password`（口令以 SHA1 摘要保存）、`+@category/-@category`（read、write、admin、pubsub、fast、slow）、`+cmd/-cmd`、`~keyglob`。
- `requirepass` 保留为 `default` 用户的口令：设置后未认证连接被拒绝；不设置时连接直接以 `default` 用户进入，ACL 规则照常生效。
- `ACL WHOAMI` 对任何已认证用户放行；`default` 用户不能删除。
- 不含 Redis 7 的多选择器与按键读写权限位，写了 `%R~` 之类规则会报未知规则。

### 上一轮已有的回移

`SET ... KEEPTTL/GET/EXAT/PXAT`、`GETEX`、`GETDEL`、四种过期命令的 `NX/XX/GT/LT`、`EXPIRETIME/PEXPIRETIME`、`SMISMEMBER`、`ZMSCORE`，以及过期时间的 64 位溢出检查。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群。持久化仍是底座的单文件 AOF 和原有 RDB；网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。命令语义参照 Redis `7.0.15`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

25 项回归全部通过：新增命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟、RDB/AOF 重启、COPY 深拷贝、ACL 权限。测试使用临时目录和随机端口，不连接业务 Redis。

## 文件

- `Redis-x64-5.0.14.1-bp1.zip`：可执行文件、许可证、说明与 SHA256 校验文件
- `Redis-x64-5.0.14.1-bp1-source.zip`：对应源码
- `Redis-x64-5.0.14.1-bp1-archives.sha256`：两个压缩包的 SHA256

部署与日常操作见仓库内的 [OPS.md](https://github.com/briantzhang/redis-server/blob/main/OPS.md)，完整范围说明见 [BACKPORT-RELEASE.md](https://github.com/briantzhang/redis-server/blob/main/BACKPORT-RELEASE.md)。
