# Package + cook smoke (M6 landmine check).
#
# 1) BuildCookRun Win64 client (Development by default; pass -Config Shipping for ship path)
# 2) Locate packaged PaintForge.exe
# 3) Boot it briefly (-nullrhi -unattended) and require a clean exit or smoke markers
#
# Usage:
#   .\Deploy\playtest\smoke-package.ps1
#   .\Deploy\playtest\smoke-package.ps1 -Config Shipping
#   .\Deploy\playtest\smoke-package.ps1 -SkipCook   # re-test an existing package
param(
	[string]$ProjectRoot = "",
	[string]$Engine = "C:\Program Files\Epic Games\UE_5.6",
	[string]$Config = "Development",
	[string]$ArchiveDir = "",
	[string]$Map = "/Game/Maps/L_Graybox",
	[int]$BootTimeoutSec = 90,
	[switch]$SkipCook
)

$ErrorActionPreference = "Stop"
if (-not $ProjectRoot) {
	$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}
if (-not $ArchiveDir) {
	$ArchiveDir = Join-Path $ProjectRoot "Packaged\Playtest"
}

$UProject = Join-Path $ProjectRoot "PaintForge.uproject"
$RunUAT = Join-Path $Engine "Engine\Build\BatchFiles\RunUAT.bat"
if (-not (Test-Path $UProject)) { throw "Missing $UProject" }
if (-not (Test-Path $RunUAT)) { throw "Missing $RunUAT" }

# --- 1) Cook / package ---
if (-not $SkipCook) {
	Write-Host "==> BuildCookRun Win64 $Config (client-only, Launcher UE friendly)"
	New-Item -ItemType Directory -Force -Path $ArchiveDir | Out-Null
	$UatArgs = @(
		"BuildCookRun",
		"-project=`"$UProject`"",
		"-noP4",
		"-platform=Win64",
		"-clientconfig=$Config",
		"-build",
		"-cook",
		"-stage",
		"-pak",
		"-archive",
		"-archivedirectory=`"$ArchiveDir`"",
		"-utf8output"
	)
	Write-Host "    Archive: $ArchiveDir"
	# Stream UAT output live (redirect buffers can deadlock long cooks).
	Push-Location $ProjectRoot
	try {
		& $RunUAT @UatArgs
		$UatCode = $LASTEXITCODE
	} finally {
		Pop-Location
	}
	if ($UatCode -ne 0) {
		Write-Host "FAIL: BuildCookRun exit $UatCode"
		exit 1
	}
	Write-Host "OK: cook/package finished"
} else {
	Write-Host "==> SkipCook: using existing package under $ArchiveDir"
}

# --- 2) Find client exe ---
$ClientExe = Get-ChildItem $ArchiveDir -Recurse -Filter "PaintForge.exe" -ErrorAction SilentlyContinue |
	Select-Object -First 1
if (-not $ClientExe) {
	Write-Host "FAIL: no PaintForge.exe under $ArchiveDir"
	exit 1
}
Write-Host "==> Client: $($ClientExe.FullName)"

# --- 3) Boot smoke (nullrhi) ---
$BootLog = Join-Path $ProjectRoot "Saved\Logs\smoke-package-boot.log"
New-Item -ItemType Directory -Force -Path (Split-Path $BootLog) | Out-Null
if (Test-Path $BootLog) { Remove-Item $BootLog -Force }

$BootArgs = @(
	$Map,
	"-nullrhi",
	"-unattended",
	"-nosound",
	"-NoPause",
	"-NoLoadingScreen",
	"-SmokeImprovement",
	"-log",
	"-ABSLOG=`"$BootLog`""
)

Write-Host "==> Boot smoke ($BootTimeoutSec s max) with -SmokeImprovement"
$WorkDir = $ClientExe.DirectoryName

# Seed arenas for packaged ProjectSavedDir (usually <package>/Windows/.../Saved).
$SeedJson = @'
{
  "schema": 1,
  "game": "PaintForge",
  "matchId": "package-smoke-seed",
  "createdUtc": "2026-07-12T00:00:00Z",
  "teamSize": 2,
  "grid": { "cellUU": 400, "subUU": 100, "wallH": 300, "cellsX": 16, "cellsY": 10, "levels": 4 },
  "arenaId": "smoke",
  "halfHashA": "smoke",
  "halfHashB": "smoke",
  "pieces": [
    { "id": 1, "t": 1, "x": 8,  "y": 16, "z": 0, "r": 0, "own": 0, "team": 0 },
    { "id": 2, "t": 1, "x": 40, "y": 16, "z": 0, "r": 0, "own": 0, "team": 1 }
  ]
}
'@
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
# Arenas moved to the stable per-user dir (the old 3-root Saved\Arenas loop existed because the packaged
# ProjectSavedDir was ambiguous — moot now).
$ArenasDir = Join-Path $env:LOCALAPPDATA "CombatForge\Arenas"
New-Item -ItemType Directory -Force -Path $ArenasDir | Out-Null
Set-Content -Path (Join-Path $ArenasDir ("arena_{0}_packagesmoke.json" -f $Stamp)) -Value $SeedJson -Encoding UTF8

$Boot = Start-Process -FilePath $ClientExe.FullName -ArgumentList $BootArgs -WorkingDirectory $WorkDir -PassThru
$Done = $Boot.WaitForExit($BootTimeoutSec * 1000)
if (-not $Done) {
	Write-Host "==> Boot timeout - killing packaged process"
	try { Stop-Process -Id $Boot.Id -Force -ErrorAction SilentlyContinue } catch {}
	Start-Sleep -Seconds 2
}

if (-not (Test-Path $BootLog)) {
	# Packaged builds often write next to the exe or under Saved relative to cwd
	$Alt = @(
		(Join-Path $WorkDir "PaintForge.log"),
		(Join-Path $WorkDir "Saved\Logs\PaintForge.log"),
		(Join-Path $ProjectRoot "Saved\Logs\PaintForge.log")
	) | Where-Object { Test-Path $_ } | Select-Object -First 1
	if ($Alt) { $BootLog = $Alt }
}

Write-Host "==> Scanning boot log: $BootLog"
if (-not (Test-Path $BootLog)) {
	# Still a useful landmine check if cook produced an exe that at least started then died.
	if ($Done -and $Boot.ExitCode -eq 0) {
		Write-Host "PASS: package cook + clean boot exit (no ABSLOG captured)"
		exit 0
	}
	Write-Host "FAIL: no boot log and no clean exit (exit=$($Boot.ExitCode))"
	exit 2
}

$Text = Get-Content $BootLog -Raw -ErrorAction SilentlyContinue
$Pass = $Text -match "SMOKE: Improvement PASS"
$Fail = $Text -match "SMOKE: Improvement FAIL"
$Started = $Text -match "LogInit|LogLoad|PaintForge|GameMode"
$Fatal = $Text -match "Fatal error|Assertion failed|Ensure condition failed"

if ($Pass -and -not $Fail) {
	Write-Host "PASS: package cook + Improvement boot smoke"
	exit 0
}

Select-String -Path $BootLog -Pattern "SMOKE:|Fatal|Error:|Assertion|Improvement loaded|LogInit" |
	Select-Object -Last 25 |
	ForEach-Object { Write-Host ("    " + $_.Line) }

if ($Fail -or $Fatal) {
	Write-Host "FAIL: packaged boot smoke"
	exit 1
}

if ($Started -and $Done) {
	Write-Host "PASS: package cook + packaged process started (no Improvement marker)"
	exit 0
}

Write-Host "FAIL: packaged boot inconclusive"
exit 2
