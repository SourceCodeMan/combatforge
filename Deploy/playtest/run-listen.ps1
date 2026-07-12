# LISTEN host — this PC is server AND a player (simplest playtest).
#
# Priority: packaged/staged game -> Development game -> Unreal Editor -game ?Listen
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

$GameExe = Find-PaintForgeExe $ProjectRoot "PaintForge.exe"
$Editor = Get-UnrealEditor $Engine
$ListenUrl = "${Map}?Listen"

$Exe = $null
$Args = @()
$WorkDir = $ProjectRoot
$Mode = ""

if (-not $PreferEditor -and $GameExe) {
	$Mode = "game-listen"
	$Exe = $GameExe
	$WorkDir = Split-Path $Exe -Parent
	$Args = @($ListenUrl, "-port=$Port", "-log")
	if ($Exe -match "[\\/]Binaries[\\/]Win64[\\/]") {
		$Args += "-project=$UProject"
	}
}
elseif ($Editor) {
	$Mode = "editor-listen"
	$Exe = $Editor
	$WorkDir = $ProjectRoot
	# -game = play-in-standalone (not PIE multi-window); ?Listen makes this machine the host.
	$Args = @(
		$UProject,
		$ListenUrl,
		"-game",
		"-log",
		"-port=$Port",
		"-WINDOWED",
		"-ResX=1600",
		"-ResY=900"
	)
}
else {
	throw "No host available. Install UE 5.6 or build/package PaintForge.exe."
}

if ($ExtraArgs) { $Args += $ExtraArgs }

Write-Host "==> PaintForge LISTEN host ($Mode) — you play on this machine"
Write-Host "    Exe:  $Exe"
Write-Host "    URL:  $ListenUrl"
Write-Host "    Port: $Port"
Write-JoinBanner -Port $Port

Set-Location $WorkDir
& $Exe @Args
