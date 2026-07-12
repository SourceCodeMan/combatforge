# Cook + stage + archive a Windows CLIENT for friends (and host).
# Editor must be closed. First cook can take a long time.
#
# Note: -server cook requires Server target support (source engine). On Launcher UE we
# package the client only; host with run-listen.ps1 / run-server.ps1 using that client.
param(
	[string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
	[string]$Config = "Development",
	[string]$Engine = "C:\Program Files\Epic Games\UE_5.6",
	[string]$ArchiveDir = "",
	[switch]$TryServer
)

$ErrorActionPreference = "Stop"
$UProject = Join-Path $ProjectRoot "PaintForge.uproject"
$RunUAT = Join-Path $Engine "Engine\Build\BatchFiles\RunUAT.bat"
if (-not $ArchiveDir) {
	$ArchiveDir = Join-Path $ProjectRoot "Packaged\Playtest"
}

if (-not (Test-Path $UProject)) { throw "Project not found: $UProject" }
if (-not (Test-Path $RunUAT)) { throw "RunUAT.bat not found: $RunUAT" }

New-Item -ItemType Directory -Force -Path $ArchiveDir | Out-Null

$UatArgs = @(
	"BuildCookRun",
	"-project=$UProject",
	"-noP4",
	"-platform=Win64",
	"-clientconfig=$Config",
	"-build",
	"-cook",
	"-stage",
	"-pak",
	"-archive",
	"-archivedirectory=$ArchiveDir",
	"-utf8output"
)

if ($TryServer) {
	Write-Host "==> Including -server (needs source engine Server target support)"
	$UatArgs += @("-server", "-serverplatform=Win64", "-serverconfig=$Config")
} else {
	Write-Host "==> Client-only package (Launcher-engine friendly)"
}

Write-Host "==> BuildCookRun -> $ArchiveDir"
Write-Host "    Config=$Config  (editor must be closed)"

& $RunUAT @UatArgs
if ($LASTEXITCODE -ne 0) { throw "Package failed ($LASTEXITCODE)" }

$ClientDir = Join-Path $ArchiveDir "Windows"
$ConnectSrc = Join-Path $PSScriptRoot "connect.ps1"
if ((Test-Path $ClientDir) -and (Test-Path $ConnectSrc)) {
	Copy-Item $ConnectSrc (Join-Path $ClientDir "connect.ps1") -Force
	# Also copy if nested
	Get-ChildItem $ClientDir -Directory -ErrorAction SilentlyContinue | ForEach-Object {
		Copy-Item $ConnectSrc (Join-Path $_.FullName "connect.ps1") -Force -ErrorAction SilentlyContinue
	}
	Write-Host "Copied connect.ps1 into client package"
}

Write-Host "OK: packaged under $ArchiveDir"
Get-ChildItem $ArchiveDir -Recurse -Filter "PaintForge.exe" -ErrorAction SilentlyContinue |
	ForEach-Object { Write-Host "  CLIENT: $($_.FullName)" }

Write-Host ""
Write-Host "Host:   .\Deploy\playtest\run-listen.ps1"
Write-Host "        .\Deploy\playtest\run-server.ps1"
Write-Host "Friends: zip the Windows client folder + connect.ps1"
