param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('install','uninstall','start','stop','status','restart')]
    [string]$Action,
    [string]$Name = 'RedisBackport',
    [string]$Config,
    [string]$Directory = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'
$server = Join-Path $Directory 'redis-server.exe'
$cli = Join-Path $Directory 'redis-cli.exe'
if (-not (Test-Path -LiteralPath $server -PathType Leaf)) {
    Write-Error "redis-server.exe not found in $Directory"
    exit 2
}
if (-not $Config) { $Config = Join-Path $Directory 'redis.windows.conf' }
$configPath = [System.IO.Path]::GetFullPath($Config)
$dataPath = [System.IO.Path]::GetFullPath((Join-Path $Directory 'data'))
if (-not (Test-Path -LiteralPath $dataPath)) { New-Item -ItemType Directory -Path $dataPath | Out-Null }

function Require-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Error 'Run this script from an elevated PowerShell session.'
        exit 3
    }
}

function Service-Args {
    return @('--service-run', '--service-name', $Name, $configPath, '--dir', $dataPath)
}

function Wait-Stopped {
    param([int]$Seconds = 15)
    for ($i = 0; $i -lt $Seconds; $i++) {
        $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
        if (-not $service -or $service.Status -eq 'Stopped') { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

switch ($Action) {
    'install' {
        Require-Admin
        if (Get-Service -Name $Name -ErrorAction SilentlyContinue) {
            Write-Error "Service $Name already exists"
            exit 4
        }
        & $server --service-install $configPath --service-name $Name --dir $dataPath
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & sc.exe config $Name start= auto | Out-Null
        Write-Host "Installed $Name (auto start, config $configPath)"
    }
    'uninstall' {
        Require-Admin
        $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
        if (-not $service) { Write-Error "Service $Name does not exist"; exit 5 }
        if ($service.Status -ne 'Stopped') { Write-Error 'Stop the service before uninstall'; exit 6 }
        & $server --service-uninstall --service-name $Name
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Host "Uninstalled $Name"
    }
    'start' {
        Require-Admin
        & $server --service-start --service-name $Name
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        if (Test-Path -LiteralPath $cli -PathType Leaf) {
            $port = Select-String -Path $configPath -Pattern '^\s*port\s+(\d+)' |
                    Select-Object -First 1
            $probePort = if ($port) { $port.Matches[0].Groups[1].Value } else { '6379' }
            for ($i = 0; $i -lt 20; $i++) {
                & $cli -h 127.0.0.1 -p $probePort PING 2>$null | Out-Null
                if ($LASTEXITCODE -eq 0) { Write-Host "Started $Name"; exit 0 }
                Start-Sleep -Milliseconds 500
            }
            Write-Error "Service started but PING failed on port $probePort"
            exit 7
        }
        Write-Host "Started $Name"
    }
    'stop' {
        Require-Admin
        & $server --service-stop --service-name $Name
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        if (-not (Wait-Stopped)) { Write-Error "Service $Name did not stop"; exit 8 }
        Write-Host "Stopped $Name"
    }
    'restart' {
        Require-Admin
        & $server --service-stop --service-name $Name
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        if (-not (Wait-Stopped)) { Write-Error "Service $Name did not stop"; exit 8 }
        & $server --service-start --service-name $Name
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Host "Restarted $Name"
    }
    'status' {
        $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
        if (-not $service) { Write-Host "$Name is not installed"; exit 9 }
        Write-Host "$Name is $($service.Status)"
    }
}
