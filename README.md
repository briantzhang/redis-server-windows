# Redis for Windows 5.0.14.1-bp1

Windows x64 上的单机 Redis，以 [tporadowski/redis](https://github.com/tporadowski/redis) 的
`v5.0.14.1`（提交 `ec77f72dc`，2022-02 发布）为底座，选择性回移了 Redis 6.0–7.0 的一批单机命令。

它不是 Redis 官方发行版，也不是 Redis 6、7 或 8。`redis-server --version` 报告的版本号是 `5.0.14.1-bp1`。

## 这个版本新增了什么

| 命令 | 上游引入版本 | 行为 |
| --- | --- | --- |
| `SET key value [NX\|XX] [GET] [EX\|PX\|EXAT\|PXAT time\|KEEPTTL]` | 6.0 / 6.2；NX+GET 取自 7.0 | 覆盖时可保留原 TTL，可原子返回旧值，可按绝对时间过期 |
| `GETEX key [EX\|PX\|EXAT\|PXAT time\|PERSIST]` | 6.2 | 原子读取并更新或去掉 TTL；不带选项时不改 TTL |
| `GETDEL key` | 6.2 | 原子读取并删除字符串 |
| `EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT` 的 `NX\|XX\|GT\|LT` | 7.0 | 按条件更新 TTL；允许 XX+GT、XX+LT，拒绝冲突组合 |
| `EXPIRETIME key` / `PEXPIRETIME key` | 7.0 | 查询绝对过期时间；键不存在返回 -2，没有 TTL 返回 -1 |
| `SMISMEMBER key member [member ...]` | 6.2 | 一次判断多个集合成员 |
| `ZMSCORE key member [member ...]` | 6.2 | 一次查询多个有序集合分数 |

过期时间在秒数乘 1000、相对时间加当前时间之前做有符号 64 位溢出检查，非法值报错且不改动数据。

命令语义参照 Redis `7.0.15`（BSD）手工适配，没有引入 Redis 8 的源码。完整说明见 [BACKPORT-RELEASE.md](BACKPORT-RELEASE.md)。

## 没有合入的部分

以下能力不在本版中，调用会返回 `unknown command` 或沿用底座的旧行为：

- RESP3、客户端缓存追踪（`CLIENT TRACKING`）、Redis Functions
- ACL、TLS、线程化 I/O
- 多段 AOF（`appenddirname`）、新版 RDB 格式
- 集群、Redis 8 的查询引擎

持久化仍是底座的单文件 AOF 和原有 RDB。带 TTL 的写入在 AOF 里记成普通 `SET` 加一条绝对时间的 `PEXPIREAT`，重放时不会把相对过期时间再算一遍。

## 构建

需要 Windows x64、Visual Studio 2022 的 C++ Build Tools（工具集 v143）和 Windows SDK。工程文件已从底座的 v142 改为 v143。

```powershell
.\build-tools\build-windows.ps1 -BuildTools 'D:\BuildTools\VS2022' -Toolset 'v143'
```

产物在 `msvs\x64\Release`：`redis-server.exe`、`redis-cli.exe`、`redis-benchmark.exe`。

只有 Windows x64 这一个构建。底座的工程文件、Win32 网络层和 jemalloc 都按 x64 编写，没有 Windows ARM64 目标，改平台配置编不出来。需要 ARM64 时只能在 Linux 上编译官方源码，不在本仓库范围内。

## 验证

在本机（Windows 11、MSVC 14.44 / v143）用当前源码编译后，22 项回归全部通过，测试使用临时目录和随机端口，不连接业务 Redis：

```bash
python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe
```

覆盖新命令、非法参数、过期边界、原子性、WATCH/MULTI、原有命令冒烟，以及 RDB 和 AOF 重启。

这轮验证不等于已合入上游全部安全补丁，也没有做生产压力测试。

## 运行

启停、持久化、备份恢复和排查见 [OPS.md](OPS.md)。

## 许可证

本仓库初始提交带的是 Apache-2.0 的 `LICENSE`，那不是这份代码的许可证。代码本身沿用底座的 BSD 三条款许可证，见 [COPYING](COPYING)，各源文件头部保留原著作权声明。hiredis、jemalloc、lua 各自携带自己的许可证文件。
