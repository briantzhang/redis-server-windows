# Redis for Windows 5.0.14.1-bp17

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp17`。

本版在 bp16 之上做收尾：去掉一处 ACL 死代码，并记录安全补丁盘点与性能基线。没有新增命令。

## 相对 bp16 的改动

### ACL 频道规则

`ACLSetUser` 里 `allchannels`、`resetchannels` 与 `&pattern` 被处理了两次。第二次无条件返回成功，使「`allchannels` 之后再加频道模式」这一本应拒绝的规则被静默接受。删除第二段，行为与 Redis 6 一致：该组合返回错误，`resetchannels` 之后才能重新添加模式。

## 安全补丁盘点

对照 Redis 5.0.14 之后、7.2.5 之前修复、且落在本版已回移命令上的公告，在 Windows x64 构建上均不成立：

| 公告 | 命令 | 结论 |
| --- | --- | --- |
| CVE-2023-22458、CVE-2023-25155 | `HRANDFIELD` / `ZRANDMEMBER` / `SRANDMEMBER` | 本版实现用 64 位 `PORT_LONG` 保存 count，负值经 `-(l+1)+1` 转换，不存在上游 32 位 `long` 上的溢出 |
| CVE-2022-35977 | `SETRANGE`、`SORT` | `SETRANGE` 在分配前用 `checkStringLength` 拒绝超过 512MB 的结果；`SORT` 的长度取自实际元素个数并收窄到 `int` |
| CVE-2022-31144、CVE-2022-35951 | `XAUTOCLAIM` | `COUNT` 经 `getRangeLongFromObjectOrReply` 限制在 `PORT_LONG_MAX/sizeof(streamID)` 以内 |
| CVE-2023-36824、CVE-2023-41053 | `COMMAND GETKEYS`、`SORT_RO` | 仅影响 7.0 及以上的键规格解析，本版不使用该解析 |
| CVE-2023-28425 | `MSETNX` | 仅影响 7.0.8 及以上 |

没有引入任何上游补丁代码。未逐条验证 Lua、HyperLogLog 与网络层的更早公告，不能据此认为已覆盖全部安全补丁。

## 性能基线

本机回环、单客户端、无 AOF，32 个元素一批，取三轮中位数（逻辑操作每秒）：

| 场景 | 顺序调用 | 流水线 | 批量命令 |
| --- | --- | --- | --- |
| 集合成员判断 | 35,195 | 448,676 | `SMISMEMBER` 744,273 |
| 有序集分数查询 | 34,036 | 355,443 | `ZMSCORE` 498,605 |
| 读旧值再写入 | 17,528（两条命令） | — | `SET GET` 33,416 |

只反映这台机器的回环数字，不代表生产吞吐。

## 不包含

RESP3、Functions、TLS、线程化 I/O、多段 AOF、新版 RDB、集群、Sentinel、客户端内存驱逐。没有引入 Redis 8 源码。只有 Windows x64。

## 验证

黑盒回归覆盖上述 ACL 规则，并保留 bp16 及更早测试。使用独立临时目录和随机端口，不连接现有实例。
