#!/usr/bin/env python3
"""Run native regressions and retain the complete result before packaging."""
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
server = root/'msvs'/'x64'/'Release'/'redis-server.exe'
log = root/'build-tools'/'regression-windows.log'
args = [sys.executable,'-I',str(root/'tests'/'backport_regression.py'),'--server',str(server)]
with log.open('w',encoding='utf-8') as output:
    process = subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                               text=True,encoding='utf-8',errors='replace')
    for line in process.stdout:
        print(line,end='',flush=True)
        output.write(line)
        output.flush()
    code = process.wait()
if code:
    raise SystemExit(code)
dist = root/'dist'
dist.mkdir(exist_ok=True)
bench = dist/'benchmark-windows.json'
subprocess.run([sys.executable,'-I',str(root/'tests'/'backport_benchmark.py'),
                '--server',str(server),'--output',str(bench)],check=True)
version = subprocess.check_output([str(server),'--version'],text=True).strip()
report = f'''# Windows 本机验证报告

时间：{datetime.now(timezone.utc).isoformat()}

服务器：`{version}`

- Windows x64 原生 MSVC Release 构建；使用原有 Windows 平台层。
- 黑盒回归结果：通过（完整项目列表及执行时间见 regression-windows.log）。
- 测试为每项创建临时目录和回环随机端口，涵盖命令、边界、事务、并发、RDB 与 AOF 重启。
- 性能数据：benchmark-windows.json；本机回环、单客户端、无 AOF，取三轮中位数。
- 性能对比仅说明批量请求与逐条/流水线请求的区别，不能外推真实部署的整体吞吐。
- 尚未完成：RuoYi/Redisson 全链路联调、长期运行压力测试、异常断电恢复、后台保存高并发压力、Windows 服务开机自启实机验证。
- 安全边界：本次是功能回移，不声明已补齐 Redis 5 之后全部安全修复；无集群升级。
'''
(dist/'verification-windows.md').write_text(report,encoding='utf-8')
print('Native verification completed:', report.splitlines()[2],flush=True)
