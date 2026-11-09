# Redis for Windows 5.0.14.1-bp4

本版本以 tporadowski/redis 的 `v5.0.14.1` 标签为底座，选择性回移 Redis 6.0–7.0 的单机命令能力。它是自维护 Windows 移植版，不是 Redis 官方 6、7 或 8 发行版。

## 本次范围

| 能力 | 上游引入版本 | 本版行为与价值 |
| --- | --- | --- |
| `SET ... KEEPTTL` | 6.0 | 覆盖值时保留过期时间，避免客户端读 TTL 再写入的竞态 |
| `SET ... GET` | 6.2；支持 NX+GET 的语义取自 7.0 | 原子返回旧值并更新；遇到非字符串报错且不覆盖 |
| `SET ... EXAT/PXAT` | 6.2 | 接收绝对秒/毫秒过期时间 |
| `GETEX key [EX/PX/EXAT/PXAT time / PERSIST]` | 6.2 | 原子读取值并更新或去除 TTL；无选项时不更改 TTL |
| `GETDEL key` | 6.2 | 原子读取并删除字符串；并发争抢只能一个请求获得值 |
| 四种过期命令的 `NX/XX/GT/LT` | 7.0 | 条件更新 TTL；允许 XX+GT 或 XX+LT，拒绝冲突组合 |
| `EXPIRETIME/PEXPIRETIME` | 7.0 | 查询绝对过期时间；不存在返回 -2，无 TTL 返回 -1 |
| `SMISMEMBER key member ...` | 6.2 | 批量集合成员判断，减少请求次数，支持原有 intset/哈希表编码 |
| `ZMSCORE key member ...` | 6.2 | 批量有序集合分数查询，RESP2 返回字符串分数和空值 |
| `ZUNION` / `ZINTER` | 6.2 | 多键并集与交集，只返回不落库，支持 WEIGHTS 与 AGGREGATE |
| `ZDIFF` / `ZDIFFSTORE` | 6.2 | 差集；STORE 形式写入目标键 |
| `COPY` | 6.2 | 深拷贝字符串、列表、集合、有序集合、哈希和流，目标拥有独立存储与独立 TTL |
| `RESET` | 6.2 | 连接回到 0 号库并清除客户端状态 |
| `LPOS` | 6.0 | 查找列表元素下标，支持 RANK、COUNT、MAXLEN |
| `LMOVE` | 6.2 | 原子地在两个列表间移动元素，源和目标可相同 |
| `SINTERCARD` / `ZINTERCARD` | 7.0 | 只计算交集基数，支持 LIMIT 提前结束 |
| `ZADD ... GT\|LT` | 6.2 | 条件更新分数，新成员仍然直接加入 |
| `HRANDFIELD` / `ZRANDMEMBER` | 6.2 | 随机取样，支持正负 count 与 WITHVALUES/WITHSCORES |
| `LPOP` / `RPOP` 的 `count` | 6.2 | 批量弹出 |
| `BLMOVE` | 6.2 | 阻塞式原子移动 |
| 统一 `ZRANGE` / `ZRANGESTORE` | 6.2 | BYSCORE/BYLEX/REV/LIMIT，旧范围命令走同一实现 |
| `lazyfree-lazy-user-del` / `lazyfree-lazy-user-flush` | 6.0 / 6.2 | 让 DEL 与 FLUSHDB/FLUSHALL 默认走异步释放，避免大键删除阻塞事件循环 |
| ACL（`SETUSER`/`DELUSER`/`LIST`/`WHOAMI`，`AUTH user pass`） | 6.0 | 用户、口令、命令分类与键通配符；见下方范围说明 |
| 过期时间边界检查 | 参考 7.0 实现 | 在秒乘 1000、相对时间加当前时间之前检查有符号 64 位溢出 |

ACL 停在 Redis 6：一个用户一套权限，支持 `on/off`、`>password`（SHA1 摘要保存）、`+@category/-@category`、`+cmd/-cmd`、`~keyglob`。没有 Redis 7 的多选择器，也没有按键的读写权限位。`requirepass` 成为 `default` 用户的口令，设置后未认证连接被拒绝；不设置时连接以 `default` 用户进入。`ACL WHOAMI` 对任何已认证用户放行。

代码参照 Redis `7.0.15`（BSD）中的相关命令语义，按旧版 API 手工适配；没有引入 Redis 8 的源码或模块。

## 持久化适配

- 保留原有 RDB 格式、单文件 AOF、Windows 后台保存机制。
- 成功的带 TTL 的 SET 在 AOF 中记录为普通 SET + 绝对 PEXPIREAT；KEEPTTL 记录实际保留的截止时间。
- GETEX 的写入记录为 PEXPIREAT、PERSIST 或 DEL/UNLINK；GETDEL 记录为 DEL。
- 条件过期仅记录成功结果的绝对截止时间，恢复时不重新判断 NX/XX/GT/LT。
- 不迁入新版 RDB 格式、多段 AOF、RESP3、线程 I/O、TLS、Functions 或集群功能。ACL 只做到 Redis 6 的单权限集，没有 Redis 7 的选择器。原底座已有功能保留，但本版没有对其做现代化升级。
- 网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。

## 性能预期

本版主要减少多步操作的网络往返，并提供单命令原子性。SMISMEMBER/ZMSCORE 可将 N 条查询合为一条；收益受批量大小、网络延迟和客户端流水线方式影响。不能据此宣称整体吞吐与 Redis 6/7/8 相同，也没有更换内存分配器、数据编码或通讯层。

不能用版本号判断完整兼容性。没有合入的新版命令仍会返回 unknown command；跨版本的数据迁移应先备份，再验证所用数据类型与格式。

## 构建

需要 Windows x64、MSVC C++ Build Tools 和 Windows SDK。本机安装的是 VS2022 Build Tools（MSVC 14.44，工具集 v143），工程文件已从 v142 改为 v143。构建脚本默认工具位置为 `D:\BuildTools\VS2022`，可用参数覆盖。

```powershell
.\build-tools\build-windows.ps1 -BuildTools 'D:\BuildTools\VS2022' -Toolset 'v143'
```

产物位于 `msvs/x64/Release`。编译脚本会生成 `src/release.h`，写入底座 Git SHA 和工作区 dirty 状态；不自动提交或创建 Git 标签。

## 验证

使用 Python 标准库的黑盒测试，启动独立临时实例，不连接现有业务 Redis：

```bash
python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe
```

覆盖新命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟及 RDB/AOF 重启。2026-11-09 在 Windows 11、MSVC 14.44（v143）上跑完 30 项全部通过，包括此前会崩溃的 AOF 重启，以及新增的有序集合运算、COPY 深拷贝、ACL 权限、列表查找移动、交集基数、条件分数更新、随机取样、批量弹出、阻塞移动和统一范围查询用例。

这是一轮选择性回移，不等于已完成上游所有安全补丁或生产压力验证。生产投放仍需在业务负载下检验 Redisson 调用、后台持久化、异常断电恢复和长时间运行。

## 来源与许可证

- Windows 底座：https://github.com/tporadowski/redis/releases/tag/v5.0.14.1
- 命令参考：https://github.com/redis/redis/tree/7.0.15
- 随包保留 `COPYING` 及原有源码版权声明。
