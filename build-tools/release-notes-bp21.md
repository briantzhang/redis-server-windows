# Redis for Windows 5.0.14.1-bp21

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp21`。

本版在 bp20 之上让 `SCRIPT FLUSH ASYNC` 真正异步。

## 相对 bp20 的改动

### SCRIPT FLUSH ASYNC

- `SCRIPT FLUSH ASYNC` 换上一份空的脚本缓存，把旧的交给后台的延迟释放线程。命令立即返回，不再等待缓存释放完毕。
- `SCRIPT FLUSH SYNC` 与不带选项的 `SCRIPT FLUSH` 仍然立即清空。
- Lua 解释器本身仍在主线程关闭并重建。它不能跨线程释放，所以异步的只是脚本缓存这一份哈希表。

## 不包含

RESP3、Functions、TLS、多段 AOF、新版 RDB、集群、Sentinel。命令解析与执行仍在主线程。没有引入 Redis 8 源码。只有 Windows x64。

## 验证

80 项黑盒回归全部通过，其中 `SCRIPT FLUSH` 用例覆盖 `ASYNC`、`SYNC` 与非法选项。测试使用独立临时目录和随机端口，不连接现有实例。
