# Redis for Windows 5.0.14.1-bp82

本次在 bp81 基础上补齐选项级键权限和键规格：

- `SORT`/`SORT_RO ... STORE`、`GEORADIUS* ... STORE/STOREDIST`、`GEOSEARCHSTORE` 的目的键按写权限判定，源键按读权限判定。
- `ZADD ... GT/LT` 需要同时具备读写权限，普通 `ZADD` 仍只需写权限。
- `COMMAND GETKEYSANDFLAGS` 对这些动态键位返回实际权限，而不是命令级粗粒度标记。
- 新增黑盒测试覆盖 ACL DRYRUN 与 GETKEYSANDFLAGS 的选项级行为。
