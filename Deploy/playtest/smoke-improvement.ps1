# Smoke-test Improvement build mode (non-interactive).
#
# 1) Seeds Saved/Arenas/ with a tiny community fort (both teams)
# 2) Launches UnrealEditor -game -nullrhi -SmokeImprovement
# 3) GameMode configures Improvement, force-starts, asserts CommunityBasePieces > 0
# 4) Greps the log for SMOKE: Improvement PASS|FAIL
#
# Usage:
#   .\Deploy\playtest\smoke-improvement.ps1
param(
	[string]$ProjectRoot = "",
	[string]$Engine = "C:\Program Files\Epic Games\UE_5.6",
	[string]$Map = "/Game/Maps/L_Graybox",
	[int]$TimeoutSec = 120
)

$ErrorActionPreference = "Stop"
# Inline helpers (avoid sourcing _common.ps1 which uses host-ip angle brackets that
# some PowerShell hosts misparse when this script is nested).
if (-not $ProjectRoot) {
	$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}

$UProject = Join-Path $ProjectRoot "PaintForge.uproject"
if (-not (Test-Path $UProject)) { throw "Missing $UProject" }

$Editor = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor.exe"
if (-not (Test-Path $Editor)) { throw "UnrealEditor not found: $Editor" }

# --- Seed a community arena so Improvement has something to load ---
$ArenasDir = Join-Path $env:LOCALAPPDATA "CombatForge\Arenas"   # stable per-user dir (moved from Saved\Arenas)
New-Item -ItemType Directory -Force -Path $ArenasDir | Out-Null

# Structural X/Y are cell min-corners in sub-grid units (multiples of 4).
# Plot A cells 1..6 -> sub 4..24; Plot B cells 9..14 -> sub 36..56.
$SeedJson = @'
{
  "schema": 1,
  "game": "PaintForge",
  "matchId": "smoke-improvement-seed",
  "createdUtc": "2026-07-12T00:00:00Z",
  "teamSize": 2,
  "grid": { "cellUU": 400, "subUU": 100, "wallH": 300, "cellsX": 16, "cellsY": 10, "levels": 4 },
  "arenaId": "smoke",
  "halfHashA": "smoke",
  "halfHashB": "smoke",
  "pieces": [
    { "id": 1, "t": 1, "x": 8,  "y": 16, "z": 0, "r": 0, "own": 0, "team": 0 },
    { "id": 2, "t": 1, "x": 12, "y": 16, "z": 0, "r": 0, "own": 0, "team": 0 },
    { "id": 3, "t": 0, "x": 8,  "y": 16, "z": 0, "r": 0, "own": 0, "team": 0 },
    { "id": 4, "t": 1, "x": 40, "y": 16, "z": 0, "r": 0, "own": 0, "team": 1 },
    { "id": 5, "t": 1, "x": 44, "y": 16, "z": 0, "r": 0, "own": 0, "team": 1 },
    { "id": 6, "t": 0, "x": 40, "y": 16, "z": 0, "r": 0, "own": 0, "team": 1 },
    { "id": 7, "t": 4, "x": 10, "y": 18, "z": 0, "r": 0, "own": 0, "team": 0 },
    { "id": 8, "t": 4, "x": 42, "y": 18, "z": 0, "r": 0, "own": 0, "team": 1 }
  ]
}
'@

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$LiveSeed = Join-Path $ArenasDir ("arena_{0}_smokeimprove.json" -f $Stamp)
Set-Content -Path $LiveSeed -Value $SeedJson -Encoding UTF8
Write-Host "==> Seeded community arena: $LiveSeed"

$LogDir = Join-Path $ProjectRoot "Saved\Logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogPath = Join-Path $LogDir "smoke-improvement.log"
if (Test-Path $LogPath) { Remove-Item $LogPath -Force }

$ArgList = @(
	"`"$UProject`"",
	$Map,
	"-game",
	"-nullrhi",
	"-unattended",
	"-nosound",
	"-NoPause",
	"-NoLoadingScreen",
	"-SmokeImprovement",
	"-log",
	"-ABSLOG=`"$LogPath`""
)

Write-Host "==> Launching smoke Improvement ($TimeoutSec s max)"
Write-Host "    Editor: $Editor"
Write-Host "    Log:    $LogPath"

$Proc = Start-Process -FilePath $Editor -ArgumentList $ArgList -PassThru -WorkingDirectory $ProjectRoot
$Finished = $Proc.WaitForExit($TimeoutSec * 1000)
if (-not $Finished) {
	Write-Host "==> Timeout - killing editor"
	try { Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue } catch {}
	Start-Sleep -Seconds 2
}

if (-not (Test-Path $LogPath)) {
	$Fallback = Join-Path $LogDir "PaintForge.log"
	if (Test-Path $Fallback) { $LogPath = $Fallback }
}

Write-Host "==> Scanning log: $LogPath"
if (-not (Test-Path $LogPath)) {
	Write-Host "FAIL: no log file produced"
	exit 2
}

$Content = Get-Content -Path $LogPath -Raw -ErrorAction SilentlyContinue
if (-not $Content) {
	Write-Host "FAIL: empty log"
	exit 2
}

$Pass = $Content -match "SMOKE: Improvement PASS"
$Fail = $Content -match "SMOKE: Improvement FAIL"
$Loaded = [regex]::Matches($Content, "Improvement loaded (\d+) community pieces")
$BasePieces = [regex]::Matches($Content, "CommunityBasePieces=(\d+)")

if ($Loaded.Count -gt 0) {
	Write-Host ("    Inject log: Improvement loaded {0} community pieces" -f $Loaded[$Loaded.Count - 1].Groups[1].Value)
}
if ($BasePieces.Count -gt 0) {
	Write-Host ("    Base pieces: {0}" -f $BasePieces[$BasePieces.Count - 1].Groups[1].Value)
}

if ($Pass -and -not $Fail) {
	Write-Host "PASS: Improvement smoke"
	exit 0
}

Select-String -Path $LogPath -Pattern "SMOKE:|Improvement loaded|CommunityBasePieces|no community arena|Error|Fatal" |
	Select-Object -Last 30 |
	ForEach-Object { Write-Host ("    " + $_.Line) }

if ($Fail) {
	Write-Host "FAIL: Improvement smoke (see log)"
	exit 1
}

Write-Host "FAIL: no SMOKE result marker in log (editor may have crashed early)"
exit 2
