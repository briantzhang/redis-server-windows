# Redis for Windows 5.0.14.1-bp35

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版，在 bp34 之上加入 Redis 7.0 的
`CLUSTER ADDSLOTSRANGE` 与 `CLUSTER DELSLOTSRANGE`，让集群槽位可以按区间分配和撤回。
`redis-server --version` 报告 `v=5.0.14.1-bp35`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp34 的改动

- 新增 `CLUSTER ADDSLOTSRANGE <start> <end> [<start> <end> ...]`：把一段或多段槽位区间分配给当前节点。
- 新增 `CLUSTER DELSLOTSRANGE <start> <end> [<start> <end> ...]`：把一段或多段槽位区间从当前节点撤回。
- 与逐槽的 `CLUSTER ADDSLOTS`/`DELSLOTS` 相比，一条命令就能覆盖整段槽位，脚本和运维工具不必再展开上千个参数。
- 校验在应用前整体完成：参数个数、槽位越界、起始槽大于结束槽、`ADDSLOTSRANGE` 撞上已被占用的槽、
  `DELSLOTSRANGE` 删除未分配的槽、同一命令内重复给出同一槽，都会报错并原子地放弃整条命令，槽位状态不变。
- 分配成功后按 `ADDSLOTS` 的方式提交配置：清除该槽的 `importing` 状态，标记集群状态需要在下一个
  beforeSleep 更新，并触发配置重写。
- `CLUSTER HELP` 增加这两条命令的说明；命令文档表只登记了 `CLUSTER` 容器，无需改动。

## 与 Redis 7 的差异

- bp35 只回移这两个子命令；`CLUSTER SHARDS`、`CLUSTER LINKS` 等 7.0 的其它集群子命令在 bp35 时点没有加入（bp36 起加入 `CLUSTER SHARDS` 与 `CLUSTER LINKS`）。
- 参数个数为奇数时沿用本版 5.0 的命令分派风格，回复通用的 `Unknown CLUSTER subcommand or wrong number of arguments`，
  而不是 7.0 的 `wrong number of arguments for 'cluster|addslotsrange' command`。
- 槽位、握手、`MOVED`/`ASK` 重定向、故障转移和总线协议仍是底座的 Redis 5.0 实现。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 98 项测试通过，新增：
- 单节点集群下 `CLUSTER ADDSLOTSRANGE 0 5 10 12` 生效，`CLUSTER NODES` 显示 `0-5`、`10-12` 两段。
- 奇数参数、起始槽大于结束槽、已占用槽、同命令重复槽、越界槽分别报错，且失败后槽位分配保持原样。
- `CLUSTER DELSLOTSRANGE 2 4` 把 `0-5` 拆成 `0-1` 和 `5`，未涉及的部分不动；删除未分配槽和重复槽报错。
- `CLUSTER ADDSLOTSRANGE 0 16383` 一次性覆盖全部槽位后 `cluster_state` 变为 `ok`，`CLUSTER SLOTS` 只返回一段 `0-16383`，键可以正常读写。
- 撤回键所在槽后 `cluster_state` 变为 `fail`，读取返回 `CLUSTERDOWN`；重新分配该槽后状态恢复、数据仍在。
- 原有的三节点组网、`ADDSLOTS`、重定向和 `DELSLOTS` 后 `CLUSTERDOWN` 用例继续通过。

未提交、未 push、未打包、未发布。
