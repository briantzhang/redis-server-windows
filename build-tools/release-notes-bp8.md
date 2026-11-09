# Redis for Windows 5.0.14.1-bp8

以 tporadowski/redis `v5.0.14.1`（提交 `ec77f72dc`）为底座的 Windows x64 单机版，选择性回移 Redis 6.0–7.2 的命令能力。不是 Redis 官方 6、7 或 8 发行版；`redis-server --version` 报告 `v=5.0.14.1-bp8`。

本版在 bp7 的基础上补齐地理搜索、ACL 查询子命令、客户端定位与断开、`INFO everything`、`WAITAOF` 和 `SHUTDOWN` 选项。

## 相对 bp7 新增

### 地理搜索

- `GEOSEARCH key FROMMEMBER member|FROMLONLAT lon lat BYRADIUS radius unit|BYBOX width height unit [ASC|DESC] [COUNT n [ANY]] [WITHCOORD] [WITHDIST] [WITHHASH]`：按圆形或矩形区域搜索。必须且只能给一个起点、一个区域形状；`ANY` 必须与 `COUNT` 同时出现，允许在凑够数量后提前结束。距离按请求的单位返回，坐标与哈希按上游顺序附在成员之后。
- `GEOSEARCHSTORE destination source <同样的起点与区域选项> [ASC|DESC] [COUNT n [ANY]] [STOREDIST]`：把结果写入有序集合；默认分数为 geohash，`STOREDIST` 时分数为到起点的距离。源键不存在时删除目标键并返回 0。
- 旧命令 `GEORADIUS`/`GEORADIUSBYMEMBER` 与新命令共用同一套区域搜索代码，原有 `STORE`/`STOREDIST` 行为不变。

### ACL 查询

- `ACL CAT [category]`：列出全部分类（`keyspace`、`read`、`write`、`admin`、`pubsub`、`fast`、`slow`）或某个分类下的命令；未知分类报错。
- `ACL USERS`：列出全部用户名。
- `ACL GETUSER user`：返回 `flags`、`passwords`（SHA1 摘要）、`commands`、`keys`（带 `~` 前缀）、`channels` 的扁平数组；用户不存在返回空。
- `ACL GENPASS [bits]`：生成随机十六进制口令，默认 256 位（64 个字符），上限 4096 位。
- `ACL DRYRUN user command [arg ...]`：只检查该用户能否执行命令并访问其中的键，不执行命令；成功返回 `OK`，失败返回具体原因。

### 客户端定位与断开

- `CLIENT LIST` 与 `CLIENT INFO` 的行新增 `laddr=<服务端本地地址>` 和 `user=<认证用户>`。
- `CLIENT KILL` 新增 `LADDR <ip:port>`、`USER <username>`、`MAXAGE <seconds>` 过滤条件。`LADDR` 是服务端监听地址，同一地址上的连接都会命中；`MAXAGE` 按连接建立至今的秒数计算，负数被拒绝。多个条件可以组合，默认仍跳过发起命令的连接。

### 运维命令

- `INFO everything`：与 `INFO all` 等价，输出全部区段。
- `WAITAOF numlocal numreplicas timeout`：本版没有 7.2 的异步 fsync 完成队列。开启 AOF 时在命令内调用 `flushAppendOnlyFile` 并 `fsync`，随后立即返回 `[本机确认, 0]`；`numreplicas` 只做参数校验，副本数恒为 0，不阻塞到超时。
- `SHUTDOWN [NOSAVE|SAVE] [NOW] [FORCE] [ABORT]`：`NOW` 不等副本，`FORCE` 在保存失败时仍然退出，`ABORT` 取消已置位但尚未执行的关闭请求（没有待执行的关闭时返回错误）。选项可以组合，`SAVE` 与 `NOSAVE` 互斥。

### 底座已有

`MEMORY USAGE key [SAMPLES n]` 在 5.0.14.1 底座中已经存在，`SAMPLES 0` 也按“检查全部元素”处理，本轮只做了回归确认，没有改动实现。

## bp7 已有的能力

位范围与只读入口（`BITCOUNT`/`BITPOS` 的 `BYTE|BIT`、`BITFIELD_RO`、`LCS`、`SORT_RO`、`EVAL_RO`/`EVALSHA_RO`）、`CLIENT PAUSE`/`UNPAUSE`/`INFO`、`CONFIG GET` 多模式与 `CONFIG SET` 多参数、`COMMAND LIST`/`COMMAND GETKEYSANDFLAGS`；bp6 的流命令补齐；bp5 的 `LMPOP`/`BLMPOP`、`ZMPOP`/`BZMPOP`、`CLIENT NO-TOUCH`/`NO-EVICT`；bp4 的批量弹出、`BLMOVE`、统一 `ZRANGE`/`ZRANGESTORE`；bp3 的条件分数更新与随机取样；bp2 的 `LPOS`/`LMOVE`/交集基数/lazyfree；bp1 的 `SET` 系列选项、`GETEX`/`GETDEL`、过期命令条件、`EXPIRETIME`/`PEXPIRETIME`、`SMISMEMBER`/`ZMSCORE`、有序集合运算、`COPY`、`RESET` 和 Redis 6 范围的 ACL。

## 不包含

RESP3、客户端缓存追踪、Redis Functions、TLS、线程化 I/O、多段 AOF、新版 RDB 格式、集群、客户端内存驱逐。ACL 不含 Redis 7 的选择器与按键读写权限位；`WAITAOF` 不含副本 AOF 确认与阻塞等待。持久化仍是底座的单文件 AOF 和原有 RDB；网络/IOCP、Windows 服务与 fork 模拟层沿用底座代码。命令语义参照 Redis `7.0.15`、`7.2.5` 与 `6.2.14`（BSD）手工适配，没有引入 Redis 8 的源码。

只有 Windows x64 构建，没有 ARM64。

## 验证

55 项黑盒回归全部通过，其中新增 6 项：`GEOSEARCH`/`GEOSEARCHSTORE` 的圆形与矩形搜索、结果选项、`ANY` 与参数互斥、`STOREDIST` 和缺失源键；`ACL CAT/USERS/GETUSER/GENPASS/DRYRUN`；`CLIENT KILL` 的 `USER`/`MAXAGE`/`LADDR` 与 `CLIENT LIST` 新字段；`INFO everything` 与 AOF 开启时的 `WAITAOF`；AOF 关闭时 `WAITAOF` 报错；`SHUTDOWN` 选项校验及附加实例的真实退出。测试使用独立临时目录和随机端口，不连接现有实例。
