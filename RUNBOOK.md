# 故障演练与长跑记录手册

适用版本：`5.0.14.1-bp89`。本文是运维演练模板，不代替生产机器验证。

## 1. 演练前检查

- 确认版本：`redis-server.exe --version`。
- 确认发布包校验：`python -I build-tools/verify-checksums.py <release.zip>`。
- 记录 `INFO server`、`INFO clients`、`INFO stats`、`INFO persistence`。
- 备份配置、RDB/AOF、TLS 证书、ACL 文件。
- 明确演练窗口、回滚负责人和停止条件。

## 2. TLS 故障演练

1. 用不受信 CA 连接，应失败。
2. 省略客户端证书连接 mTLS 服务，应失败。
3. 用错误 SNI 或错误主机名连接，应失败。
4. 临时替换证书后 `CONFIG SET` 热加载，再恢复。
5. 记录错误文本、`INFO stats` 与客户端错误。

## 3. ACL 故障演练

1. 用 `ACL DRYRUN` 验证只允许读的用户不能 SET。
2. 验证 `%R~` 用户不能写 `SET ... GET` 的目标键。
3. 验证 `%W~` 用户不能 `GET` 普通键。
4. 验证 `ACL LOG` 记录被拒绝命令。
5. 修改规则后重新连接验证。

## 4. AOF/RDB 故障演练

1. 复制完整数据目录作为基线。
2. 截断 AOF 尾段后启动，记录恢复结果。
3. 损坏 manifest 后启动，记录错误与恢复步骤。
4. 复制 RDB 并制造部分截断，记录加载失败行为。
5. 使用备份恢复并跑 `DBSIZE` 与抽样键检查。

## 5. 复制与故障切换演练

1. 建立一主一副本，确认 `INFO replication`。
2. kill 副本后重启，确认全量或部分同步。
3. kill 主库后提升副本，确认新主可写。
4. 旧主恢复为副本，确认不产生双主写入。
5. 若启用 TLS replication，重复关键步骤。

## 6. 集群故障演练

1. 三节点组网并确认 `cluster_state:ok`。
2. kill 一个副本，确认写入继续。
3. kill 一个主库，观察 failover 与槽位归属。
4. 断开 cluster bus，确认 `CLUSTER LINKS` 状态。
5. 恢复网络后确认 `CLUSTER INFO`、`CLUSTER NODES`。

## 7. Windows 长跑记录模板

| 项目 | 记录 |
| --- | --- |
| 起止时间 |  |
| 版本 |  |
| 机器配置 |  |
| 客户端数 |  |
| 命令类型 |  |
| TLS/明文 |  |
| AOF 配置 |  |
| 总请求数 |  |
| P50/P99 延迟 |  |
| 最大内存 RSS |  |
| 连接错误数 |  |
| AOF rewrite 次数 |  |
| RDB BGSAVE 次数 |  |
| 最终结论 |  |

## 8. 停止条件

- 进程崩溃或无法恢复。
- 数据校验失败。
- 内存持续增长且无回落。
- 复制永久不同步。
- 集群无法恢复 `cluster_state:ok`。
- 磁盘满或 AOF/RDB 无法写入。

## 9. 演练后动作

1. 保存日志、配置、指标与结论。
2. 对比 `INFO stats` 前后差异。
3. 将发现的问题登记到版本说明或安全边界文档。
4. 如有数据损坏，保留现场并重新从备份验证恢复流程。
