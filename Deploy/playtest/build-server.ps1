# Attempt to compile CombatForgeServer (Win64).
# NOTE: Epic Launcher engine builds FAIL with:
#   "Server targets are not currently supported from this engine distribution."
# That is expected. For playtest on Launcher UE, skip this and use:
#   .\run-listen.ps1   or   .\run-server.ps1  (game exe + -server -nullrhi)
param(
	[string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
	[string]$Config = "Development",
	[string]$Engine = "C:\Program Files\Epic Games\UE_5.6"
)

$ErrorActionPreference = "Stop"
$UProject = Join-Path $ProjectRoot "CombatForge.uproject"
$BuildBat = Join-Path $Engine "Engine\Build\BatchFiles\Build.bat"

if (-not (Test-Path $UProject)) { throw "Project not found: $UProject" }
if (-not (Test-Path $BuildBat)) { throw "UE 5.6 Build.bat not found: $BuildBat" }

Write-Host "==> Building CombatForgeServer Win64 $Config"
Write-Host "    If this fails with 'Server targets are not currently supported',"
Write-Host "    use run-listen.ps1 / run-server.ps1 instead (no Server target needed)."
& $BuildBat CombatForgeServer Win64 $Config "-project=$UProject" -waitmutex
if ($LASTEXITCODE -ne 0) {
	Write-Host ""
	Write-Host "Server target build failed (common on Launcher UE)."
	Write-Host "Playtest path that works today:"
	Write-Host "  .\Deploy\playtest\run-listen.ps1"
	Write-Host "  .\Deploy\playtest\run-server.ps1"
	exit $LASTEXITCODE
}

$Exe = Join-Path $ProjectRoot "Binaries\Win64\CombatForgeServer.exe"
if (Test-Path $Exe) { Write-Host "OK: $Exe" }
