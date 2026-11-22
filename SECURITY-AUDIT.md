# 安全补丁审计边界

适用版本：`5.0.14.1-bp89`。本文件记录自维护 Windows 回移分支的安全边界，不代表已完成 Redis 5 之后全部上游安全补丁。

## 1. 审计原则

- 不拷贝 Redis 8 源码，不使用 RSALv2/SSPLv1/AGPL 代码。
- 只基于公开安全公告、当前代码行为与本分支自研修复做评估。
- 不声明完全等价 Redis 6/7/8 安全状态。
- 生产环境仍需单独做威胁建模、权限隔离、网络隔离与渗透验证。

## 2. 已知边界

| 领域 | 状态 | 说明 |
| --- | --- | --- |
| 认证 | 兼容底座 | `requirepass` 与 ACL 可用；应避免使用弱口令 |
| ACL | 部分增强 | `%R~`/`%W~`、selector、option-aware 键权限 |
| TLS | 增强 | mTLS、TLS 1.2/1.3、cipher、session、cluster bus、replication |
| CRL/OCSP | 未实现 | 证书撤销需由部署侧保证 |
| Lua/Functions | 子集实现 | 不使用未经验证的脚本来源 |
| Module 加载 | 仅诊断 | 不承诺第三方模块安全 |
| 集群总线 | TLS 可启用 | 建议生产开启 TLS cluster bus |
| 持久化文件 | 需保护权限 | RDB/AOF 包含数据，应按敏感文件处理 |
| 备份文件 | 需保护权限 | source zip 与配置备份不应公开 |

## 3. 已覆盖的风险点

- TLS 服务端和客户端证书校验路径有回归。
- `tls-auth-clients yes/optional/no` 三态有回归。
- TLS 协议版本和 cipher 限制可运行时验证。
- TLS replication 全量、部分和无盘同步有回归。
- ACL 键权限区分读、写、选项相关命令。
- `ACL DRYRUN` 用于避免权限规则误配。
- AOF manifest、truncated tail、bad manifest、legacy migration 有回归。
- `sanitize-dump-payload` 三态与 stream consumer payload 有回归。
- DUMP/RESTORE 类型兼容与错误路径有回归。
- 多段 AOF 并发重写、BGSAVE 压力和 Windows socket 抖动有回归。

## 4. 未覆盖或需部署侧处理

- Redis 5 之后全部安全公告的逐条补丁等价性。
- Redis 8 新增安全机制。
- CRL、OCSP、证书自动轮换、HSM 私钥保护。
- Windows 账户隔离、ACL 文件系统权限、防火墙策略。
- 日志脱敏、中心化审计、告警阈值。
- 生产规模 DDoS、慢客户端、连接耗尽场景。
- 第三方客户端库漏洞与 Redisson/RuoYi 全链路安全验证。

## 5. 建议部署基线

1. 仅监听必要网卡；公网暴露前强制 TLS 与认证。
2. 使用最小权限 ACL 用户，禁用默认用户的无密码访问。
3. 服务端证书使用完整链，客户端启用 CA 校验，不用 `--insecure`。
4. 集群开启 `tls-cluster yes` 并验证 bus 端口连通。
5. RDB/AOF/证书私钥目录限制为服务账户可读。
6. 定期备份并校验 checksums。
7. 开启日志集中收集，监控 `ACL LOG`、`INFO stats`、`INFO clients`。
8. 对安全公告保持人工跟踪，不把本文件视为最终安全证明。
