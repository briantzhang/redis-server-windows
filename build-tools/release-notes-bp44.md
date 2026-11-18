# Redis for Windows 5.0.14.1-bp44

基于 tporadowski/redis `v5.0.14.1` 的 Windows x64 单机版。bp44 是这一轮的收尾版本：在 bp43 的基础上做
性能对比与全量回归，把 bp29–bp44 的改动合并成一个提交。
`redis-server --version` 报告 `v=5.0.14.1-bp44`。不是 Redis 官方发行版，也不是 Redis 6、7 或 8 的完整移植。

## 相对 bp43 的改动

- 新增 `build-tools/bench-hotpaths.py`：标准库实现，启动自己的临时实例，覆盖 PING、SET/GET、GET 未命中、
  MGET×10、HSET/HGET、SADD/SMISMEMBER、ZADD/ZMSCORE、LPUSH/LRANGE、XADD、EVAL、INFO、CONFIG GET、DBSIZE、
  DUMP 与 RESTORE；支持 `--server2` 在同一次运行里交替测量两个构建，报告每项的中位数与最好一轮。
- 与 bp28 对比（同一进程交替测量、回环、单客户端、32 条命令一条流水线）：当前构建在 PING、SET/GET、
  GET 未命中、MGET、HSET/HGET、SADD/SMISMEMBER、ZADD/ZMSCORE、LPUSH/LRANGE、EVAL、INFO、CONFIG GET
  上快 5%–12%，DBSIZE 持平，没有发现回移造成的性能回退。
- `sanitize-dump-payload yes` 的深校验成本按 payload 大小线性计费：200 项列表的 `RESTORE` 吞吐约下降 9%，
  默认 `no` 不产生这项开销；`repl-diskless-load` 直接解析的 CPU/内存成本已在 `OPS.md` 中说明。
- 版本号升到 `5.0.14.1-bp44`，README、OPS、BACKPORT-RELEASE 与发布包脚本同步更新；
  `redis.conf` 补充 `aof-timestamp-enabled` 说明。

## 验证

`python -I tests/backport_regression.py --server msvs/x64/Release/redis-server.exe` 共 110 项全部通过，
日志末尾为 `OK`。性能数据见 `build-tools/bench-bp44-paired3.json` 与
`build-tools/bench-bp44-sanitize-{no,yes}.json`。

本地合并提交，未 push、未打包、未发布。
