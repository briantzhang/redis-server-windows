# Redis for Windows 5.0.14.1-bp3

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.0 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp3`。

本版在 bp2 的基础上补充了条件分数更新和随机取样命令。

## 相对 bp2 新增

### 有序集合

- `ZADD key [NX|XX] [GT|LT] [CH] [INCR] score member ...` 新增 `GT` / `LT`：只在新分数严格大于（`GT`）或严格小于（`LT`）当前分数时更新已有成员，分数相等视为不更新。新成员不受 `GT`/`LT` 限制，照常加入。`GT`/`LT` 与 `NX` 同时出现，或 `GT` 与 `LT` 同时出现，返回错误且不改动数据。
- `ZRANDMEMBER key [count [WITHSCORES]]`：随机返回成员。`count` 为正数时返回不重复成员，达到集合大小时返回全部；为负数时允许重复，绝对值是返回个数；`count 0` 返回空数组；不带 `count` 时返回单个成员或空。

### 哈希

- `HRANDFIELD key [count [WITHVALUES]]`：随机返回字段，计数语义与 `ZRANDMEMBER` 一致，`WITHVALUES` 在 RESP2 下返回 `field, value` 平铺数组。

两个随机命令都按实际编码取样：ziplist 编码按索引取，哈希表/跳表编码用 `dictGetRandomKey`，正数 `count` 用临时字典去重，不会因集合很大而复制整个键。

## bp2 已有的能力

`LPOS`、`LMOVE`、`SINTERCARD`、`ZINTERCARD`、`lazyfree-lazy-user-del`、`lazyfree-lazy-user-flush`，以及 bp1 的 `SET ... KEEPTTL/GET/EXAT/PXAT`、`GETEX`、`GETDEL`、四种过期命令的 `NX/XX/GT/LT`、`EXPIRETIME/PEXPIRETIME`、`SMISMEMBER`、`ZMSCORE`、`ZUNION`/`ZINTER`/`ZDIFF`/`ZDIFFSTORE`、`COPY`、`RESET` 和 Redis 6 范围的 ACL。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群。ACL 不含 Redis 7 的选择器与按键读写权限位。持久化仍是底座的单文件 AOF 和原有 RDB；网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。命令语义参照 Redis `7.0.15` / `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

28 项回归全部通过：新增命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟、RDB/AOF 重启、COPY 深拷贝、ACL 权限、列表查找移动、交集基数、lazyfree 配置、条件分数更新和两种编码下的随机取样。测试使用临时目录和随机端口，不连接业务 Redis。

## 文件

- `Redis-x64-5.0.14.1-bp3.zip`：可执行文件、许可证、说明与 SHA256 校验文件
- `Redis-x64-5.0.14.1-bp3-source.zip`：对应源码
- `Redis-x64-5.0.14.1-bp3-archives.sha256`：两个压缩包的 SHA256

部署与日常操作见仓库内的 [OPS.md](https://github.com/briantzhang/redis-server/blob/main/OPS.md)，完整范围说明见 [BACKPORT-RELEASE.md](https://github.com/briantzhang/redis-server/blob/main/BACKPORT-RELEASE.md)。
