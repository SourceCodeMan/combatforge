# Package + cook smoke (M6 landmine check).
#
# 1) BuildCookRun Win64 client (Development by default; pass -Config Shipping for ship path)
# 2) Locate packaged CombatForge.exe
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
. (Join-Path $PSScriptRoot "_common.ps1")
if (-not $ProjectRoot) {
	$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}
if (-not $ArchiveDir) {
	$ArchiveDir = Join-Path $ProjectRoot "Packaged\Playtest"
}

$UProject = Join-Path $ProjectRoot "CombatForge.uproject"
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

# --- 2) Find client exe (Binaries / largest file; reject the 166 KB bootstrap) ---
$ClientPath = Find-CombatForgeExe $ArchiveDir "CombatForge.exe"
if (-not $ClientPath) {
	$Hit = Get-ChildItem $ArchiveDir -Recurse -Filter "CombatForge.exe" -ErrorAction SilentlyContinue |
		Sort-Object Length -Descending | Select-Object -First 1
	if ($Hit -and $Hit.Length -ge 10MB) { $ClientPath = $Hit.FullName }
}
if (-not $ClientPath) {
	Write-Host "FAIL: no CombatForge.exe under $ArchiveDir that is at least 10 MB (166 KB bootstrap is not the game)."
	exit 1
}
$ClientExe = Get-Item $ClientPath
Write-Host ("==> Client: {0} ({1:N1} MB)" -f $ClientExe.FullName, ($ClientExe.Length / 1MB))

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
  "game": "CombatForge",
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
# Seed a smoke-only TEMP dir and pass -ArenaDir so -nullrhi cannot miss the JSON
# and cannot pollute a live fleet ProgramData map pool.
$ArenasDir = Join-Path $env:TEMP "CombatForge\SmokeArenas"
New-Item -ItemType Directory -Force -Path $ArenasDir | Out-Null
Set-Content -Path (Join-Path $ArenasDir ("arena_{0}_packagesmoke.json" -f $Stamp)) -Value $SeedJson -Encoding UTF8
$BootArgs += "-ArenaDir=`"$ArenasDir`""

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
		(Join-Path $WorkDir "CombatForge.log"),
		(Join-Path $WorkDir "Saved\Logs\CombatForge.log"),
		(Join-Path $ProjectRoot "Saved\Logs\CombatForge.log")
	) | Where-Object { Test-Path $_ } | Select-Object -First 1
	if ($Alt) { $BootLog = $Alt }
}

# --- 3b) SECURITY SCRUB, after the boot ---
# The boot smoke runs the packaged exe, which writes <package>\CombatForge\Saved back INTO the
# archive - Config, Logs, and historically a crash dump and a logged-in session token. package-
# playtest.ps1 scrubs BEFORE any smoke, so on this path the archive is dirty again by the time it
# is zipped. Doing it here means the archive this script leaves behind is always distributable,
# instead of relying on the operator remembering a manual re-scrub. (P2-D5)
# The boot log can itself live inside the package, so it is copied out before the scrub runs.
if ((Test-Path $BootLog) -and $BootLog.StartsWith($ArchiveDir, [StringComparison]::OrdinalIgnoreCase)) {
	$Kept = Join-Path $ProjectRoot "Saved\Logs\smoke-package-boot-copied.log"
	New-Item -ItemType Directory -Force -Path (Split-Path $Kept) | Out-Null
	Copy-Item $BootLog $Kept -Force -ErrorAction SilentlyContinue
	if (Test-Path $Kept) { $BootLog = $Kept }
}
Get-ChildItem $ArchiveDir -Recurse -Directory -Filter "Saved" -ErrorAction SilentlyContinue |
	Where-Object { $_.Parent.Name -eq "CombatForge" } |
	ForEach-Object {
		Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
		Write-Host "==> Scrubbed $($_.FullName) (smoke boot wrote runtime data into the package)"
	}
Get-ChildItem $ArchiveDir -Recurse -Filter *.pdb -ErrorAction SilentlyContinue |
	Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host "==> Scanning boot log: $BootLog"
if (-not (Test-Path $BootLog)) {
	Write-Host "FAIL: no boot log (a real exe must produce ABSLOG; the 166 KB bootstrap does not). exit=$($Boot.ExitCode)"
	exit 2
}

$Text = Get-Content $BootLog -Raw -ErrorAction SilentlyContinue
$Pass = $Text -match "SMOKE: Improvement PASS"
$Fail = $Text -match "SMOKE: Improvement FAIL"
$Started = $Text -match "LogInit|LogLoad|CombatForge|GameMode"
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
	# Distinct SOFT-FAIL (issue #21 D9): the process booted but the Improvement marker never appeared,
	# so the smoke did NOT prove the game content loaded. Exit 3 so automation can distinguish this
	# from a real PASS (0) and a hard fail (1).
	Write-Host "WARN: packaged process started but NO Improvement marker - content load unproven (exit 3)"
	exit 3
}

Write-Host "FAIL: packaged boot inconclusive"
exit 2
