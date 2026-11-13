# Redis for Windows 5.0.14.1-bp23

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp23`。

本版在 bp22 之上接入 RESP3。默认仍是 RESP2，客户端用 `HELLO 3` 把当前连接切过去。

## 相对 bp22 的改动

### HELLO 3

- `HELLO` 接受协议 2 和 3，其它版本仍返回 NOPROTO。协商的版本从这条回复开始生效，一直保留到下一次 `HELLO` 或 `RESET`。`RESET` 把连接退回协议 2。
- 回复是一个 map：`server`、`version`、`proto`、`id`、`mode`、`role`、`modules`。RESP2 连接上 map 仍是交替字段和值的扁平数组，旧客户端不用改。

### RESP3 类型

- 新增 map（`%`）、set（`~`）、null（`_`）、bool（`#`）、double（`,`）和 verbatim（`=`）的编码。RESP2 下 map 是翻倍长度的数组，set 是普通数组，null 仍是 `$-1` 和 `*-1`。
- 缺失值在 RESP3 下回 `_`。`GET`、`ZSCORE`、`ZMSCORE` 和 `ACL GETUSER` 查不到用户时都走这条。
- `ZSCORE` 的分数在 RESP3 下是 double，不再是字符串。
- `COMMAND DOCS`、`ACL GETUSER` 和 `CLIENT TRACKINGINFO` 的外层在 RESP3 下是 map。

## 不包含

RESP3 的 push 回复。`CLIENT TRACKING` 的失效通知仍走 Pub/Sub 频道 `__redis__:invalidate`，需要 `REDIRECT`。没有 Functions、TLS、多段 AOF、新版 RDB 和集群。命令解析与执行仍在主线程。没有引入 Redis 8 源码。只有 Windows x64。

## 验证

82 项黑盒回归全部通过。新增的用例覆盖 `HELLO 3` 的协议切换与 `RESET` 退回协议 2，以及 RESP3 下的 map、null 和 double。测试使用独立临时目录和随机端口，不连接现有实例。
