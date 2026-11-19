# Redis for Windows 5.0.14.1-bp64

本轮补齐多段 AOF 截断恢复回归：

- 在运行时开启 AOF 后，向当前增量 AOF 追加半条 RESP 命令并重启。
- 验证服务器在 `aof-load-truncated yes` 下仅恢复完整命令，并把文件截回有效长度。
- 验证截断恢复后的新写入可在下一次重启后正确读取。
- 全量共 133 项。

只提供 Windows x64 构建，未 push、未打包、未发布。
