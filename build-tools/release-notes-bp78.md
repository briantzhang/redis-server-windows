# 5.0.14.1-bp78

本版梳理 Windows x64 模块支持诊断。

- `MODULE HELP` 增加平台要求说明：仅支持兼容的 x64 DLL，且必须导出 `RedisModule_OnLoad`。
- `MODULE LOAD` 失败诊断保持与帮助文本一致，避免只在出错时才知道平台限制。
- Windows x64 Release 构建通过，140 项回归全部通过。

未 push，未发布。
