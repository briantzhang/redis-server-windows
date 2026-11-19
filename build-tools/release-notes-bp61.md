# Redis for Windows 5.0.14.1-bp61

本轮补齐客户端输出缓冲限制的配置回归：

- 覆盖 `CONFIG SET/GET client-output-buffer-limit`，验证 normal 类别的 hard、soft 与 soft-seconds 生效。
- 覆盖非法类别、负数、缺失参数被拒绝，且失败时不修改当前配置。
- 全量共 130 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
