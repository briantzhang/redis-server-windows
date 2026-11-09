# Redis for Windows 5.0.14.1-bp5

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp5`。

本版在 bp4 的基础上补齐了多键批量弹出，并加入两个客户端标志。

## 相对 bp4 新增

### 列表

- `LMPOP numkeys key [key ...] LEFT|RIGHT [COUNT count]`：依次检查键，从第一个非空列表弹出最多 `count` 个元素，回复 `[键, [元素 ...]]`；所有键都为空时回复空数组。`COUNT` 缺省为 1，必须大于 0。
- `BLMPOP timeout numkeys key [key ...] LEFT|RIGHT [COUNT count]`：`LMPOP` 的阻塞版本，在所有给定键上等待，超时支持小数秒；在 `MULTI/EXEC` 中不阻塞、立即返回空。

### 有序集合

- `ZMPOP numkeys key [key ...] MIN|MAX [COUNT count]`：从第一个非空有序集合弹出最多 `count` 个成员，回复 `[键, [[成员, 分数], ...]]`。
- `BZMPOP timeout numkeys key [key ...] MIN|MAX [COUNT count]`：`ZMPOP` 的阻塞版本，语义与 `BLMPOP` 一致。
- `ZPOPMIN`/`ZPOPMAX` 的 `count` 解析与 Redis 6.2/7.0 对齐：`0` 返回空数组，负数报 `value is out of range, must be positive`。

批量弹出在 AOF 和复制流中改写为 `LPOP`/`RPOP`/`ZPOPMIN`/`ZPOPMAX` 加 `COUNT`，从库和旧客户端不需要理解多键命令。阻塞版本被唤醒时也只记录实际弹出的元素个数。

### 客户端标志

- `CLIENT NO-TOUCH on|off`：该连接后续访问键时不更新 LRU/LFU，`TOUCH` 命令本身仍会更新。`CLIENT LIST` 的 `flags` 里显示 `T`。
- `CLIENT NO-EVICT on|off`：接受并记录“不参与客户端驱逐”的标志，`CLIENT LIST` 的 `flags` 里显示 `e`。本版没有实现 Redis 7 的客户端内存驱逐（`maxmemory-clients`），因此这个标志不会产生实际驱逐效果，只为客户端兼容而接受。
- `RESET` 会清除这两个标志。

## bp4 已有的能力

`LPOP`/`RPOP` 的 `count`、`BLMOVE`、小数秒阻塞超时、统一 `ZRANGE`/`ZRANGESTORE`；bp3 的 `ZADD ... GT|LT`、`HRANDFIELD`、`ZRANDMEMBER`；bp2 的 `LPOS`、`LMOVE`、`SINTERCARD`、`ZINTERCARD`、lazyfree 配置；bp1 的 `SET ... KEEPTTL/GET/EXAT/PXAT`、`GETEX`、`GETDEL`、过期命令的 `NX/XX/GT/LT`、`EXPIRETIME/PEXPIRETIME`、`SMISMEMBER`、`ZMSCORE`、`ZUNION`/`ZINTER`/`ZDIFF`/`ZDIFFSTORE`、`COPY`、`RESET` 和 Redis 6 范围的 ACL。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐。ACL 不含 Redis 7 的选择器与按键读写权限位。持久化仍是底座的单文件 AOF 和原有 RDB；网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。命令语义参照 Redis `7.0.15` 与 `7.2.5`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

33 项回归全部通过：新增命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟、RDB/AOF 重启、COPY 深拷贝、ACL 权限、列表查找移动、交集基数、lazyfree 配置、条件分数更新、随机取样、批量弹出、阻塞移动、统一范围查询、多键弹出和 CLIENT 标志。测试使用临时目录和随机端口，不连接业务 Redis。

## 文件

- `Redis-x64-5.0.14.1-bp5.zip`：可执行文件、许可证、说明与 SHA256 校验文件
- `Redis-x64-5.0.14.1-bp5-source.zip`：对应源码
- `Redis-x64-5.0.14.1-bp5-archives.sha256`：两个压缩包的 SHA256

部署与日常操作见仓库内的 [OPS.md](https://github.com/briantzhang/redis-server/blob/main/OPS.md)，完整范围说明见 [BACKPORT-RELEASE.md](https://github.com/briantzhang/redis-server/blob/main/BACKPORT-RELEASE.md)。
