# Redis for Windows 5.0.14.1-bp4

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.0 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp4`。

本版在 bp3 的基础上补齐了列表批量弹出、阻塞移动和统一的有序集合范围查询。

## 相对 bp3 新增

### 列表

- `LPOP key [count]` / `RPOP key [count]`：带 `count` 时返回数组，列表不足时返回剩余全部元素，清空后删除键；`count` 为 0 返回空数组，负数报错；不带 `count` 时保持原来的单元素回复。
- `BLMOVE source destination LEFT|RIGHT LEFT|RIGHT timeout`：`LMOVE` 的阻塞版本，源列表为空时等待，超时返回空。超时参数现在支持小数秒（`0.5`），`BLPOP`/`BRPOP`/`BRPOPLPUSH` 同样受益。

### 有序集合

- 统一 `ZRANGE key start stop [BYSCORE|BYLEX] [REV] [LIMIT offset count] [WITHSCORES]`：支持按分数区间、字典序区间、反向和分页。`BYSCORE` 支持 `(` 开区间与 `-inf`/`+inf`，`BYLEX` 支持 `[`、`(`、`-`、`+`。`LIMIT` 只能与 `BYSCORE`/`BYLEX` 组合，`WITHSCORES` 不能与 `BYLEX` 组合。
- `ZRANGESTORE dst src start stop [BYSCORE|BYLEX] [REV] [LIMIT offset count]`：把范围结果写入目标键并返回元素个数；结果为空时删除目标键并返回 0。
- `ZRANGE`/`ZREVRANGE`/`ZRANGEBYSCORE`/`ZREVRANGEBYSCORE`/`ZRANGEBYLEX`/`ZREVRANGEBYLEX` 改为走同一套解析和遍历实现，行为与 Redis 6.2 对齐；`ZCOUNT`、`ZLEXCOUNT` 保持不变。

## bp3 已有的能力

`ZADD ... GT|LT`、`HRANDFIELD`、`ZRANDMEMBER`，以及 bp2 的 `LPOS`、`LMOVE`、`SINTERCARD`、`ZINTERCARD`、lazyfree 配置，和 bp1 的 `SET ... KEEPTTL/GET/EXAT/PXAT`、`GETEX`、`GETDEL`、过期命令的 `NX/XX/GT/LT`、`EXPIRETIME/PEXPIRETIME`、`SMISMEMBER`、`ZMSCORE`、`ZUNION`/`ZINTER`/`ZDIFF`/`ZDIFFSTORE`、`COPY`、`RESET` 和 Redis 6 范围的 ACL。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群。ACL 不含 Redis 7 的选择器与按键读写权限位。持久化仍是底座的单文件 AOF 和原有 RDB；网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。命令语义参照 Redis `7.0.15` / `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

30 项回归全部通过：新增命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟、RDB/AOF 重启、COPY 深拷贝、ACL 权限、列表查找移动、交集基数、lazyfree 配置、条件分数更新、随机取样、批量弹出、阻塞移动和统一范围查询。测试使用临时目录和随机端口，不连接业务 Redis。

## 文件

- `Redis-x64-5.0.14.1-bp4.zip`：可执行文件、许可证、说明与 SHA256 校验文件
- `Redis-x64-5.0.14.1-bp4-source.zip`：对应源码
- `Redis-x64-5.0.14.1-bp4-archives.sha256`：两个压缩包的 SHA256

部署与日常操作见仓库内的 [OPS.md](https://github.com/briantzhang/redis-server/blob/main/OPS.md)，完整范围说明见 [BACKPORT-RELEASE.md](https://github.com/briantzhang/redis-server/blob/main/BACKPORT-RELEASE.md)。
