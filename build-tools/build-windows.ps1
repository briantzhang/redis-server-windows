param([string]$BuildTools = 'D:\BuildTools\VS2022', [string]$Toolset = 'v143')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$msbuild = Join-Path $BuildTools 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw 'MSVC Build Tools not installed. Install C++ workload, v142 and Windows SDK first.' }
$sha = (& git -C $root rev-parse --short=8 HEAD).Trim()
$dirty = if (& git -C $root diff --name-only) { '1' } else { '0' }
$buildId = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$hdr = @"
#define REDIS_GIT_SHA1 "$sha"
#define REDIS_GIT_DIRTY "$dirty"
#define REDIS_BUILD_ID "windows-bp1-$buildId"
"@
[IO.File]::WriteAllText((Join-Path $root 'src\release.h'), $hdr, [Text.Encoding]::ASCII)
$log = Join-Path $root 'build-tools\build-windows.log'
& $msbuild (Join-Path $root 'msvs\RedisServer.sln') /m:4 /p:Configuration=Release /p:Platform=x64 "/p:PlatformToolset=$Toolset" /nologo /v:minimal "/flp:LogFile=$log;Verbosity=normal"
if ($LASTEXITCODE -ne 0) { throw "Windows build failed (exit $LASTEXITCODE). Log: $log" }
Write-Output "Build succeeded. Executables: $root\msvs\x64\Release"
