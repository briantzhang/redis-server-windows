# 升级与迁移指南

适用版本：从 `5.0.14.1-bp86`、`5.0.14.1-bp87`、`5.0.14.1-bp88` 升级到 `5.0.14.1-bp89`。

## 1. 升级前检查

1. 用 `redis-server.exe --version` 确认当前版本。
2. 备份 RDB、AOF 目录、配置文件、TLS 证书、ACL 文件。
3. 记录当前配置：`CONFIG GET *`、`ACL LIST`、`CLUSTER NODES`、`INFO replication`。
4. 确认目标机器是 Windows x64；本分支不提供 ARM64 构建。

## 2. 单机升级

1. 停止旧服务或进程，确认端口已释放。
2. 解压新发布包到独立目录，保留 OpenSSL DLL 与 exe 同目录。
3. 复制旧配置，按新版本说明检查新增配置项。
4. 启动新二进制并观察日志。
5. 执行 `PING`、`INFO server`、`DBSIZE` 和抽样读写验证。

如果开启 AOF 多段目录，不要手工重命名 manifest 或 base/incr 文件。升级前先停服并复制整个 `appendonlydir/`。

## 3. bp86 → bp87

- 新增 redis-cli / redis-benchmark / hiredis TLS 工具链。
- `rediss://` URI 支持 `cacert`、`sni`、`cert`、`key`、`insecure`、`tls` 查询参数。
- ACL 键权限与 `COMMAND GETKEYSANDFLAGS` 按选项和键位解析。
- FUNCTION、RDB/DUMP、RESP3、集群可观测性回归补强。

注意：客户端 TLS 参数不会自动从服务端配置读取，需在 CLI 命令显式提供。

## 4. bp87 → bp88

- 修复 redis-cli 断线重连时丢失原对端地址的问题。
- TLS 集群 CLI 测试扩展到 `info`/`check`/`call` 与 `rediss://`。
- 动态键规格的 GETKEYSANDFLAGS 覆盖补充。
- OPS/README/发布说明/打包版本同步。

注意：`--cluster import` 源端断线后会重连原源端地址，不再回到 CLI 主连接地址。

## 5. bp88 → bp89

- 补齐 COMMAND、ACL、AOF、RESP3、TRACKING、TLS、复制、Stream、运维、发布工程化回归与文档。
- `verify-checksums.py` 支持直接校验 zip。
- `package-windows.py` 支持 `--dry-run`。
- 新增 `COMPATIBILITY.md`、`UPGRADING.md`、`SECURITY-AUDIT.md` 和分发清单模板。

注意：新增能力以回归与文档为主；没有引入 Redis 8 源码。

## 6. TLS 配置迁移

服务端配置示例：

```text
port 0
tls-port 6380
tls-cert-file D:/redis/certs/server.crt
tls-key-file D:/redis/certs/server.key
tls-ca-cert-file D:/redis/certs/ca.crt
tls-auth-clients yes
```

客户端示例：

```powershell
.\redis-cli.exe --tls --cacert ca.crt -h redis.example -p 6380 PING
.\redis-cli.exe --tls --insecure -p 6380 --cluster info 127.0.0.1:6380
.\redis-benchmark.exe --tls --insecure -p 6380 -t ping
```

切换 `tls-auth-clients`、`tls-protocols`、cipher 和 session caching 可运行时生效；证书路径和 `tls-port` 变更需要重启。

## 7. ACL 用户迁移

1. 先用 `ACL LIST` 保存用户规则。
2. 用 `ACL DRYRUN <user> <command> ...` 验证关键命令。
3. 对 `%R~` / `%W~` 键模式，分别验证读、写和选项相关命令。
4. 再执行 `ACL SAVE` 或写入 aclfile。

注意：选项相关命令如 `SET ... GET`、`GETEX ... EX`、`SORT ... STORE`、`ZADD GT/LT` 会按实际参数判断读写权限。

## 8. 集群升级

1. 逐个节点滚动升级，先升级副本再升级主库。
2. 每次只重启一个节点，观察 `CLUSTER INFO`、`CLUSTER NODES`、`CLUSTER LINKS`。
3. TLS cluster 同时确认 `tls-cluster yes` 与 bus 端口连通。
4. 不要在升级过程中手工改槽位归属。

## 9. 回滚

1. 停止新进程。
2. 确认新版本未生成无法被旧版本读取的数据格式；如不确定，保留新旧目录和数据副本。
3. 使用旧二进制和旧配置启动。
4. 验证 `INFO persistence`、`DBSIZE` 与抽样业务键。

## 10. 发布校验

```powershell
python -I build-tools/verify-checksums.py dist/Redis-x64-5.0.14.1-bp89.zip
python -I build-tools/package-windows.py --dry-run
```
