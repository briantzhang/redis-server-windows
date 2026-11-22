param(
    [Parameter(Mandatory=$true)][string]$ReleaseZip,
    [Parameter(Mandatory=$true)][string]$InstallDir,
    [string]$ServiceName = 'Redis',
    [switch]$SkipService,
    [switch]$SkipVerify
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $scriptDir

if (-not (Test-Path -LiteralPath $ReleaseZip -PathType Leaf)) {
    throw "Release zip not found: $ReleaseZip"
}
if (-not $SkipVerify) {
    $python = Get-Command python -ErrorAction Stop
    & $python.Source '-I' (Join-Path $scriptDir 'verify-checksums.py') $ReleaseZip
    if ($LASTEXITCODE -ne 0) { throw 'Release checksum verification failed' }
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("redis-bootstrap-" + [guid]::NewGuid().ToString('N'))
try {
    Expand-Archive -LiteralPath $ReleaseZip -DestinationPath $temp -Force
    $checksums = Get-ChildItem -Path $temp -Filter checksums.sha256 -Recurse | Select-Object -First 1
    if ($null -eq $checksums) { throw 'Release package has no checksums.sha256' }
    $payload = $checksums.DirectoryName
    Copy-Item -Path (Join-Path $payload '*') -Destination $InstallDir -Recurse -Force
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}

$server = Join-Path $InstallDir 'redis-server.exe'
if (-not (Test-Path -LiteralPath $server -PathType Leaf)) { throw 'redis-server.exe missing after extraction' }
& $server '--version'

if (-not $SkipService) {
    $admin = Join-Path $InstallDir 'service-admin.ps1'
    if (-not (Test-Path -LiteralPath $admin -PathType Leaf)) {
        $admin = Join-Path $scriptDir 'service-admin.ps1'
    }
    Write-Host "Service install is available via: $admin"
}

Write-Host "Bootstrap completed: $InstallDir"
