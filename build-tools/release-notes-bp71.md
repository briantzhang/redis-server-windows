# Redis for Windows 5.0.14.1-bp71

本轮补齐客户端与模块能力的明确诊断：

- 随包 redis-cli 只支持明文连接；传入 `--tls`、`rediss://` 等参数时会明确提示不支持 TLS，而不是尝试明文握手。
- `MODULE LOAD` 失败时给出 Windows x64 DLL 与 `RedisModule_OnLoad` 的诊断信息。
- 更新 OPS 与 README 对模块和 CLI TLS 限制的描述。
- 新增 MODULE LOAD 诊断回归。
- 全量共 138 项，打包前使用当前构建重新跑通。

只提供 Windows x64 构建，未 push 到 tporadowski/redis。
