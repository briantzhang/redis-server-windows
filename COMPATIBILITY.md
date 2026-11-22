# Redis for Windows 兼容性矩阵

适用版本：`5.0.14.1-bp89`（Windows x64）。底座是 tporadowski/redis `v5.0.14.1`。本文件记录本自维护回移分支的实际兼容范围，不代表 Redis 官方发行版，也不代表 Redis 7/8 全量功能。

## 1. 总体边界

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| Redis 5.0.14.1 单机命令 | 兼容 | 底座命令保留；新增命令为选择性回移 |
| Redis 6.0–7.2 命令 | 部分兼容 | 仅矩阵中列出的命令/选项已实现并有回归 |
| Redis 8 源码 | 不使用 | 不拷贝 RSALv2/SSPLv1/AGPL 代码 |
| Windows x64 | 支持 | MSVC v143 Release 构建 |
| Windows ARM64 | 不支持 | 未提供构建与发布包 |
| Lua scripting | 兼容底座 | Functions 为自维护子集 |
| Modules | 仅诊断 | 不承诺加载第三方 Redis Module |
| 集群 | 单/多节点可用 | 基于 Redis 5 集群实现，补观测与 TLS |

## 2. 命令兼容矩阵

| 命令/选项 | 上游版本 | 本分支状态 | 回归覆盖 |
| --- | --- | --- | --- |
| `SET ... KEEPTTL/GET/EXAT/PXAT` | 6.0/6.2 | 已实现 | 有 |
| `GETEX` / `GETDEL` | 6.2 | 已实现 | 有 |
| `EXPIRETIME` / `PEXPIRETIME` | 7.0 | 已实现 | 有 |
| 四种过期命令 `NX/XX/GT/LT` | 7.0 | 已实现 | 有 |
| `SMISMEMBER` / `ZMSCORE` | 6.2 | 已实现 | 有 |
| `ZUNION` / `ZINTER` / `ZDIFF` | 6.2 | 已实现 | 有 |
| `COPY` / `RESET` | 6.2 | 已实现 | 有 |
| `LPOS` / `LMOVE` / `BLMOVE` | 6.0/6.2 | 已实现 | 有 |
| `LMPOP` / `BLMPOP` | 7.0 | 已实现 | 有 |
| `ZMPOP` / `BZMPOP` | 7.0 | 已实现 | 有 |
| 统一 `ZRANGE` / `ZRANGESTORE` | 6.2 | 已实现 | 有 |
| `HRANDFIELD` / `ZRANDMEMBER` | 6.2 | 已实现 | 有 |
| `COMMAND LIST` / `DOCS` / `GETKEYSANDFLAGS` | 6.x/7.x | 子集实现 | 有 |
| `CLIENT` 扩展选项 | 6.x/7.x | 子集实现 | 有 |
| `CONFIG GET/SET` 多参数 | 7.0 | 已实现 | 有 |
| `FUNCTION LOAD/LIST/DELETE/DUMP/RESTORE/KILL/STATS` | 7.0 | 子集实现 | 有 |
| `FCALL` / `FCALL_RO` | 7.0 | 已实现 | 有 |
| `WAITAOF` | 7.0 | 已实现 | 有 |
| `OBJECT HELP` 扩展 | 6.x/7.x | 子集实现 | 有 |

## 3. 协议兼容矩阵

| 协议/格式 | 状态 | 说明 |
| --- | --- | --- |
| RESP2 | 完整支持 | 默认协议 |
| RESP3 map/set/null/bool/double/verbatim | 支持 | `HELLO 3` 后可用 |
| RESP3 push 帧 | 支持 | Pub/Sub、无效化消息使用 `>` |
| `HELLO` 认证信息 | 支持 | 返回 negotiated proto、username、mode |
| Redis 7/8 HELLO 全字段 | 不承诺 | 仅保留本分支已实现字段 |
| DUMP/RDB 版本 | 兼容底座 | 支持现有 payload 版本回归矩阵 |

## 4. TLS 兼容矩阵

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| 服务端 TLS 监听 | 支持 | `tls-port`、CA/证书/私钥 |
| mTLS `tls-auth-clients yes/optional/no` | 支持 | 运行时可切换 |
| TLS 1.2 / 1.3 与 cipher 配置 | 支持 | `CONFIG SET` 可热应用 |
| TLS cluster bus | 支持 | `tls-cluster yes` |
| TLS replication | 支持 | 全量、部分、无盘复制 |
| redis-cli `--tls` | 支持 | CA、cert、key、SNI、insecure |
| redis-benchmark `--tls` | 支持 | 同步驱动模式 |
| hiredis async TLS API | 支持 | `redisAsyncConnectTLS` |
| CRL / OCSP | 未实现 | 不在当前回移范围 |
| Redis 8 TLS 新语义 | 不承诺 | 不拷贝上游 8 源码 |

## 5. AOF/RDB/持久化矩阵

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| 单文件 AOF | 兼容底座 | 启动时可加载 |
| 多段 AOF | 支持 | manifest/base/incr 布局 |
| AOF 运行时开关 | 支持 | 回归覆盖 |
| `redis-check-aof` 多段检查 | 支持 | 逐版本回归覆盖 |
| AOF 截断恢复 | 支持 | 有界故障注入回归 |
| RDB 10/11 payload | 支持 | 版本兼容矩阵覆盖 |
| `sanitize-dump-payload` | 支持 | 三态回归覆盖 |
| 断电级掉电恢复 | 未完全证明 | 仅做可控 kill/截断回归 |

## 6. 集群兼容矩阵

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| Redis 5 cluster 命令 | 支持 | MEET/ADDSLOTS/DELSLOTS 等 |
| `CLUSTER SHARDS` / `LINKS` / `MIGRATION` | 支持 | 观测命令回移 |
| `CLUSTER SLOT-STATS` | 支持 | 含 SLOTSRANGE 过滤 |
| `CLUSTER ADDSLOTSRANGE` / `DELSLOTSRANGE` | 支持 | 区间槽位管理 |
| 主机名宣告 | 支持 | `cluster-announce-hostname` |
| TLS cluster bus | 支持 | 双端口与证书校验 |
| Redis 7/8 集群完整迁移工具 | 不承诺 | CLI 仅覆盖已列子命令路径 |

## 7. Windows 运维矩阵

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| Windows 服务管理脚本 | 支持 | `build-tools/service-admin.ps1` |
| 校验脚本 | 支持 | 目录与 zip |
| 打包脚本 | 支持 | dry-run、版本检查、manifest、SHA256 |
| 引导部署脚本 | 提供 | `build-tools/bootstrap-windows.ps1` |
| winget/Scoop 清单 | 提供模板 | 发布后填入真实哈希 |
| 长跑/压力 | 有界覆盖 | 不代替生产机器长跑 |
