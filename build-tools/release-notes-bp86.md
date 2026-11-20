# Redis for Windows 5.0.14.1-bp86

本次补齐 Windows 运维与稳定性验证工具：

- 发布包新增 `service-admin.ps1`，封装服务安装、启动、停止、重启、状态和卸载，带管理员权限检查、健康检查与退出码。
- 发布包新增 `verify-checksums.py`，可校验解压目录的完整性和多余文件。
- OPS.md 增加服务脚本、日志轮转和校验用法；验证报告更新未验证范围。
- 新增有界回归 `test_aof_multipart_concurrent_rewrites`：多段 AOF 连续重写与 BGSAVE 期间持续写入，结束后校验数据以及持久化状态。
- 打包脚本随 ZIP 发布上述工具，并在包内 README 中说明。

全量 Windows 原生回归在提交前执行。
