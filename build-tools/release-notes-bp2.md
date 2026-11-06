# Redis for Windows 5.0.14.1-bp2

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.0 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp2`。

本版在 bp1 的基础上补充了列表操作、交集基数、异步释放配置和编译优化。

## 相对 bp1 新增

### 列表

- `LPOS key element [RANK rank] [COUNT n] [MAXLEN m]`：查找元素下标。负 `RANK` 从尾部查找，`COUNT 0` 返回全部匹配，`MAXLEN` 限制扫描长度。
- `LMOVE source destination LEFT|RIGHT LEFT|RIGHT`：原子地在两个列表间移动元素，源和目标可以是同一个列表。

### 交集基数

- `SINTERCARD numkeys key [key ...] [LIMIT n]`：只返回集合交集基数，不构建结果集。
- `ZINTERCARD numkeys key [key ...] [LIMIT n]`：只返回有序集合交集基数，忽略分数。
- 两者都按 numkeys 解析全部键，ACL 的键通配符对每个键逐一检查；`LIMIT` 只影响提前结束，不改变返回值。

### 内存释放

- `lazyfree-lazy-user-del yes`：`DEL` 等价于 `UNLINK`，大键删除走后台线程。
- `lazyfree-lazy-user-flush yes`：不带 `ASYNC` 的 `FLUSHDB`/`FLUSHALL` 也走后台释放。
- 两项都支持 `CONFIG SET`、`CONFIG GET` 和 `CONFIG REWRITE`。

### 构建

- Release 配置开启内部函数（`/Oi`）和更积极的内联（`/Ob2`），保留 `/O2`、LTCG 和速度优先。

## bp1 已有的能力

`SET ... KEEPTTL/GET/EXAT/PXAT`、`GETEX`、`GETDEL`、四种过期命令的 `NX/XX/GT/LT`、`EXPIRETIME/PEXPIRETIME`、`SMISMEMBER`、`ZMSCORE`、`ZUNION`/`ZINTER`/`ZDIFF`/`ZDIFFSTORE`、`COPY`、`RESET`，以及 Redis 6 范围的 ACL（`SETUSER`/`DELUSER`/`LIST`/`WHOAMI`、`AUTH user pass`、命令分类与键通配符）。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群。ACL 不含 Redis 7 的选择器与按键读写权限位。持久化仍是底座的单文件 AOF 和原有 RDB；网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。命令语义参照 Redis `7.0.15`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

27 项回归全部通过：新增命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟、RDB/AOF 重启、COPY 深拷贝、ACL 权限、列表查找移动、交集基数和 lazyfree 配置。测试使用临时目录和随机端口，不连接业务 Redis。

## 文件

- `Redis-x64-5.0.14.1-bp2.zip`：可执行文件、许可证、说明与 SHA256 校验文件
- `Redis-x64-5.0.14.1-bp2-source.zip`：对应源码
- `Redis-x64-5.0.14.1-bp2-archives.sha256`：两个压缩包的 SHA256

部署与日常操作见仓库内的 [OPS.md](https://github.com/briantzhang/redis-server/blob/main/OPS.md)，完整范围说明见 [BACKPORT-RELEASE.md](https://github.com/briantzhang/redis-server/blob/main/BACKPORT-RELEASE.md)。
