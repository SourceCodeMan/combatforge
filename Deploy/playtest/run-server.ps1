# Headless-style playtest SERVER (no local player on this process).
# ASCII-only so Windows PowerShell 5.1 parses cleanly.
#
# Priority:
#   1) PaintForgeServer.exe
#   2) Packaged/staged PaintForge.exe -server -nullrhi
#   3) Development PaintForge.exe -server -nullrhi
#   4) UnrealEditor.exe -server -nullrhi
param(
	[string]$ProjectRoot = "",
	[int]$Port = 7777,
	[string]$Map = "/Game/Maps/L_Graybox",
	[string]$Engine = "C:\Program Files\Epic Games\UE_5.6",
	[switch]$NoFirewall,
	[switch]$PreferEditor,
	[string]$ExtraArgs = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")
if (-not $ProjectRoot) { $ProjectRoot = Get-ProjectRoot $PSScriptRoot }

$UProject = Join-Path $ProjectRoot "PaintForge.uproject"
if (-not (Test-Path $UProject)) { throw "Missing $UProject" }

if (-not $NoFirewall) { Ensure-PlaytestFirewall -Port $Port }

$ServerExe = Find-PaintForgeExe $ProjectRoot "PaintForgeServer.exe"
$GameExe = Find-PaintForgeExe $ProjectRoot "PaintForge.exe"
$Editor = Get-UnrealEditor $Engine

$Exe = $null
$Args = @()
$WorkDir = $ProjectRoot
$Mode = ""

if (-not $PreferEditor -and $ServerExe) {
	$Mode = "dedicated-binary"
	$Exe = $ServerExe
	$WorkDir = Split-Path $Exe -Parent
	$Args = @($Map, "-log", "-port=$Port", "-NOHOMEDIR")
}
elseif (-not $PreferEditor -and $GameExe) {
	$Mode = "game-server"
	$Exe = $GameExe
	$WorkDir = Split-Path $Exe -Parent
	$Args = @($Map, "-server", "-nullrhi", "-nosound", "-log", "-port=$Port", "-NOHOMEDIR")
	if ($Exe -match "[\\/]Binaries[\\/]Win64[\\/]") {
		$Args += "-project=$UProject"
	}
}
elseif ($Editor) {
	$Mode = "editor-server"
	$Exe = $Editor
	$WorkDir = $ProjectRoot
	$Args = @(
		$UProject,
		$Map,
		"-server",
		"-log",
		"-port=$Port",
		"-nullrhi",
		"-nosound",
		"-nosplash",
		"-unattended"
	)
}
else {
	throw "No server host available. Install UE 5.6 editor or place a PaintForge.exe build."
}

if ($ExtraArgs) { $Args += $ExtraArgs }

Write-Host "==> PaintForge playtest SERVER ($Mode)"
Write-Host "    Exe:  $Exe"
Write-Host "    Map:  $Map"
Write-Host "    Port: $Port"
Write-JoinBanner -Port $Port
Write-Host "Ctrl+C or close the window to stop the server."
Write-Host ""

Set-Location $WorkDir
& $Exe @Args
$Code = $LASTEXITCODE
if ($Code -ne 0 -and $null -ne $Code) {
	Write-Host "Server exited with code $Code"
	if ($Mode -eq "game-server") {
		Write-Host "Tip: try editor host instead:"
		Write-Host "  .\Deploy\playtest\run-server.ps1 -PreferEditor"
	}
	exit $Code
}
