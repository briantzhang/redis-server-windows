# 分发渠道清单

版本：`5.0.14.1-bp89`。本文件列出模板和发布后的实际渠道信息。模板中的哈希占位符应在发布脚本产出最终归档后填入。

## 1. 主发布渠道

- GitHub Releases（权威发布）
  - 仓库：`briantzhang/redis-server-windows`
  - 标签：`v5.0.14.1-bp89`
  - Release URL：`https://github.com/briantzhang/redis-server-windows/releases/tag/v5.0.14.1-bp89`

## 2. 发布资产

| 文件 | 用途 | SHA256 |
| --- | --- | --- |
| `Redis-x64-5.0.14.1-bp89.zip` | Windows x64 二进制发布包 | 发布后由 `*-archives.sha256` 提供 |
| `Redis-x64-5.0.14.1-bp89-source.zip` | 自维护源码归档 | 发布后由 `*-archives.sha256` 提供 |
| `Redis-x64-5.0.14.1-bp89-archives.sha256` | 两个归档的 SHA256 | 同上 |

校验：

```powershell
python -I build-tools/verify-checksums.py dist/Redis-x64-5.0.14.1-bp89.zip
```

## 3. winget 模板

可在发布后由 `build-tools/generate-channel-manifests.py` 生成。示例字段：

```yaml
PackageIdentifier: briantzhang.RedisServerWindows
PackageVersion: 5.0.14.1-bp89
PackageLocale: en-US
Publisher: briantzhang
PackageName: Redis Server for Windows
License: BSD-3-Clause
ShortDescription: Self-maintained Windows x64 Redis backport
InstallerType: zip
Installers:
  - Architecture: x64
    Url: https://github.com/briantzhang/redis-server-windows/releases/download/v5.0.14.1-bp89/Redis-x64-5.0.14.1-bp89.zip
    Sha256: REPLACE_WITH_RELEASE_SHA256
ManifestType: singleton
ManifestVersion: 1.6.0
```

## 4. Scoop 模板

```json
{
  "version": "5.0.14.1-bp89",
  "architecture": {
    "64bit": {
      "url": "https://github.com/briantzhang/redis-server-windows/releases/download/v5.0.14.1-bp89/Redis-x64-5.0.14.1-bp89.zip",
      "hash": "REPLACE_WITH_RELEASE_SHA256"
    }
  },
  "bin": ["redis-server.exe", "redis-cli.exe", "redis-benchmark.exe"],
  "checkver": "github: briantzhang/redis-server-windows"
}
```

## 5. Chocolatey 模板

包脚本应下载发布 zip 并用哈希校验，然后安装到 `tools/`。不要嵌入私钥或账号信息。

## 6. 离线镜像

- 将三个发布资产复制到内部镜像。
- 保留 `checksums.sha256` 与包内 `manifest.json`。
- 分发公告中同时写明 GitHub Release URL、版本、SHA256 与适用平台。
- 不要发布未校验的构建产物。

## 7. 通道更新流程

1. 构建并本地验证。
2. 打包并生成归档 SHA256。
3. 推送 `publish` 分支与标签。
4. 创建 GitHub Release 并上传资产。
5. 用最终哈希更新渠道清单。
6. 发布公告中列出资产与校验命令。
