# CombatForge - run a FLEET of match instances on one box (up to 3 active matches).
#
# Each instance is its own CombatForge server process on its own UDP port (7777, 7778, ...),
# registering with the directory under the shared ServerKey - the api keys each row as
# <serverId>#<port>, so all of them show in the in-game SERVERS list at once. Players who click
# START NEW MATCH in the game join the first EMPTY instance and set it up.
#
# USAGE (on the box, from the game folder next to Start-Server-OnBox.ps1):
#   powershell -ExecutionPolicy Bypass -File .\Start-Fleet-OnBox.ps1 -Instances 3
#
# SIZING - each headless UE instance costs roughly 1-1.5 GB RAM and a share of CPU.
#   vc2-4c-8gb (the current Vultr box): 3 instances is the realistic ceiling (playtest lag).
#   Do not raise the hard cap without resizing the box first.
#
# Stop: close the spawned windows (each has its own restart loop), or re-run this script /
# Deploy-OnBox (they now kill the leftover restart hosts).
#
# NOTE: this file is deliberately ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI unless
# there is a BOM, so smart quotes / em-dashes turn into mojibake and break parsing.

param(
    [int]$Instances = 3,
    [int]$BasePort = 7777,
    [string]$Map = "L_Graybox"
)

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Single = Join-Path $Here "Start-Server-OnBox.ps1"
if (-not (Test-Path $Single)) { throw "Start-Server-OnBox.ps1 not found next to this script." }

if ($Instances -lt 1) { $Instances = 1 }
if ($Instances -gt 3) {
    Write-Host "Capping at 3 instances (box lag ceiling; playtest 2026-08-06)." -ForegroundColor Yellow
    $Instances = 3
}

$mem = $null
try { $mem = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB } catch {}
if ($mem) {
    $fit = [Math]::Max(1, [Math]::Floor(($mem - 2) / 1.25))
    Write-Host ("Box RAM: {0:n1} GB - comfortable fleet size ~{1} instance(s)." -f $mem, $fit) -ForegroundColor Cyan
    if ($Instances -gt $fit) {
        Write-Host ("WARNING: asking for {0} instances likely OVERCOMMITS this box. Watch for paging/hitches." -f $Instances) -ForegroundColor Yellow
    }
}

function Stop-OnBoxRestartHosts {
    # Never touch $PID (this fleet launch). Same sweep as Deploy-OnBox.
    $self = $PID

    $logDir = Join-Path $Here "Logs"
    foreach ($name in @("host.pid", "host2.pid", "host3.pid")) {
        $pf = Join-Path $logDir $name
        if (Test-Path $pf) {
            $oldPid = 0
            try { $oldPid = [int]((Get-Content $pf -Raw).Trim()) } catch { $oldPid = 0 }
            if ($oldPid -gt 0 -and $oldPid -ne $self) {
                Stop-Process -Id $oldPid -Force -ErrorAction SilentlyContinue
            }
        }
    }

    $cim = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ProcessId -ne $self -and
            $_.Name -match '^(powershell|pwsh)\.exe$' -and
            $_.CommandLine -and
            $_.CommandLine -match 'Start-Server-OnBox\.ps1|Start-Fleet-OnBox\.ps1|Deploy-OnBox\.ps1'
        }
    foreach ($p in $cim) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }

    Get-Process -Name powershell,pwsh -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $self -and $_.MainWindowTitle -match 'CombatForge server #' } |
        Stop-Process -Force -ErrorAction SilentlyContinue

    Get-Process -Name "CombatForge*" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    $deadline = (Get-Date).AddSeconds(20)
    do {
        Start-Sleep -Seconds 1
        $psLeft = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
            Where-Object {
                $_.ProcessId -ne $self -and
                $_.Name -match '^(powershell|pwsh)\.exe$' -and
                $_.CommandLine -and
                $_.CommandLine -match 'Start-Server-OnBox\.ps1|Start-Fleet-OnBox\.ps1'
            }).Count
        $titleLeft = @(Get-Process -Name powershell,pwsh -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -ne $self -and $_.MainWindowTitle -match 'CombatForge server #' }).Count
        $exeLeft = @(Get-Process -Name "CombatForge*" -ErrorAction SilentlyContinue).Count
    } while (($psLeft + $titleLeft + $exeLeft) -gt 0 -and (Get-Date) -lt $deadline)

    if (($psLeft + $titleLeft + $exeLeft) -gt 0) {
        throw "Old restart-loop host(s) still running (ps=$psLeft title=$titleLeft exe=$exeLeft). Close those windows and re-run."
    }
}

Write-Host ""
Write-Host ("=== CombatForge fleet: {0} instance(s), ports {1}-{2} ===" -f $Instances, $BasePort, ($BasePort + $Instances - 1)) -ForegroundColor Cyan
Write-Host "Each instance opens in its own window with its own restart loop." -ForegroundColor DarkGray
Write-Host ""

Write-Host "Stopping leftover restart hosts..." -ForegroundColor Gray
Stop-OnBoxRestartHosts

for ($i = 1; $i -le $Instances; $i++) {
    $port = $BasePort + $i - 1
    Write-Host ("[{0}/{1}] starting instance on port {2}..." -f $i, $Instances, $port) -ForegroundColor Green
    Start-Process powershell -ArgumentList @(
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$Single`"",
        "-Port", "$port",
        "-InstanceIndex", "$i",
        "-Map", "`"$Map`""
    )
    Start-Sleep -Seconds 4   # stagger boots so the VC++ check / firewall adds don't race
}

Write-Host ""
Write-Host "Fleet launched. Each window shows its own 'Backend: fleet registered (port N)'." -ForegroundColor Cyan
Write-Host "All instances then appear in the in-game SERVERS list (scroll for more rows)." -ForegroundColor Cyan
