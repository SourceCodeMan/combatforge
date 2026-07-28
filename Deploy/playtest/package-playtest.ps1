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
$UProject = Join-Path $ProjectRoot "CombatForge.uproject"
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

# COOK-FLAG DELTA vs Scripts/Package-Windows.bat, on purpose (P2-D11): that script adds
# -iostore -compressed -nodebuginfo -prereqs for a shipping-shaped package. This one is the
# fast local playtest loop - a lighter cook that iterates quicker and keeps symbols for triage.
# Anything that goes to itch goes through Package-Windows.bat, so the shipped layout is the
# bat's, not this one's. Align the flags here only if a playtest needs a byte-identical cook.
Write-Host "==> BuildCookRun -> $ArchiveDir"
Write-Host "    Config=$Config  (editor must be closed)"

& $RunUAT @UatArgs
if ($LASTEXITCODE -ne 0) { throw "Package failed ($LASTEXITCODE)" }

$ClientDir = Join-Path $ArchiveDir "Windows"

# SECURITY SCRUB (durable fix for the 2026-07-17 token leak): a packaged build run from a writable folder
# writes runtime data into <package>\CombatForge\Saved - INCLUDING a logged-in session token on pre-fix
# builds. That must NEVER be distributed. Also drop debug PDBs (size). The per-user auth-path fix already
# keeps fresh logins out of the package, but this guarantees a stray login or crash dump can't ship.
$SavedDir = Join-Path $ClientDir "CombatForge\Saved"
if (Test-Path $SavedDir) { Remove-Item $SavedDir -Recurse -Force -ErrorAction SilentlyContinue; Write-Host "Scrubbed $SavedDir (never ship runtime login/crash data)" }
Get-ChildItem $ClientDir -Recurse -Filter *.pdb -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

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
Get-ChildItem $ArchiveDir -Recurse -Filter "CombatForge.exe" -ErrorAction SilentlyContinue |
	ForEach-Object { Write-Host "  CLIENT: $($_.FullName)" }

Write-Host ""
Write-Host "Host:   .\Deploy\playtest\run-listen.ps1"
Write-Host "        .\Deploy\playtest\run-server.ps1"
Write-Host "Friends: zip the Windows client folder + connect.ps1"
