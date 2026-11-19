# Redis for Windows 5.0.14.1-bp58

本轮补齐对象元信息命令回归：

- 覆盖 `OBJECT HELP`、`ENCODING`、`REFCOUNT`，以及不存在键与非法子命令返回值。
- 覆盖 LFU 策略下 `FREQ` 可用、`IDLETIME` 被拒绝；切回非 LFU 策略后行为相反。
- 全量共 127 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
