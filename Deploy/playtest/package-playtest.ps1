# Cook + stage + archive a Windows CLIENT for friends (and host).
# Editor must be closed. First cook can take a long time.
#
# Note: -server cook requires Server target support (source engine). On Launcher UE we
# package the client only; host with run-listen.ps1 / run-server.ps1 using that client.
param(
	[string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
	[ValidateSet("Development", "Shipping")]
	[string]$Config = "Development",
	[string]$Engine = "C:\Program Files\Epic Games\UE_5.6",
	[string]$ArchiveDir = "",
	[switch]$TryServer,
	[switch]$AllowSameProtocol
)

$ErrorActionPreference = "Stop"
$UProject = Join-Path $ProjectRoot "CombatForge.uproject"
$RunUAT = Join-Path $Engine "Engine\Build\BatchFiles\RunUAT.bat"
if (-not $ArchiveDir) {
	$ArchiveDir = if ($Config -eq "Shipping") {
		Join-Path $ProjectRoot "Packaged\Release"
	} else {
		Join-Path $ProjectRoot "Packaged\Playtest"
	}
}

if (-not (Test-Path $UProject)) { throw "Project not found: $UProject" }
if (-not (Test-Path $RunUAT)) { throw "RunUAT.bat not found: $RunUAT" }

$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$ArchiveDir = [IO.Path]::GetFullPath($ArchiveDir)
$PackagedRoot = [IO.Path]::GetFullPath((Join-Path $ProjectRoot "Packaged"))
if (-not $ArchiveDir.StartsWith($PackagedRoot + [IO.Path]::DirectorySeparatorChar,
	[StringComparison]::OrdinalIgnoreCase)) {
	throw "ArchiveDir must be a child of $PackagedRoot (got $ArchiveDir)"
}

# Store packages must never inherit stale files from an older archive. The target is validated above
# and is intentionally narrow before any recursive deletion occurs.
if ($Config -eq "Shipping" -and (Test-Path $ArchiveDir)) {
	Write-Host "==> Removing prior Shipping archive $ArchiveDir"
	Remove-Item -LiteralPath $ArchiveDir -Recurse -Force
}
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

if ($Config -eq "Shipping") {
	$UatArgs += @(
		"-clean",
		"-distribution",
		"-iostore",
		"-compressed",
		"-nodebuginfo",
		"-prereqs"
	)
}

if ($TryServer) {
	Write-Host "==> Including -server (needs source engine Server target support)"
	$UatArgs += @("-server", "-serverplatform=Win64", "-serverconfig=$Config")
} else {
	Write-Host "==> Client-only package (Launcher-engine friendly)"
}

# THIS SCRIPT IS THE SHIP PATH, despite the "playtest" name. alpha-15, alpha-16 and alpha-17 were
# all cooked here and butler-pushed from Packaged\Playtest\Windows; Packaged\Windows (the
# Scripts\Package-Windows.bat output) does not even exist on the build machine. The stale
# "run Scripts\Package-Windows.bat" line in docs/itch-deploy.md is the wrong one, not this.
#
# This is the single Windows packaging implementation. Scripts/Package-Windows.bat is now only a
# compatibility wrapper around this file. Shipping adds clean/distribution/IoStore/compression/
# prerequisites/no-debug-info above; Development keeps the fast playtest shape.
# Optional HARD GATE (P2-S4). PF_REQUIRE_PROTOCOL_BUMP=1 refuses to cook at a NetProtocol value
# a previous successful package already shipped at. The gate originally went on
# Scripts\Package-Windows.bat, which turned out NOT to be the path that ships - so it lives here
# too, on the one that does.
$ProtoStamp = Join-Path $ProjectRoot "Packaged\.last-packaged-protocol"
$ProtoMatch = Select-String -Path (Join-Path $ProjectRoot "Source\CombatForge\CombatForge.h") `
    -Pattern 'constexpr\s+int32\s+NetProtocol\s*=\s*(\d+)\s*;'
$Proto = if ($ProtoMatch) { $ProtoMatch.Matches[0].Groups[1].Value } else { "" }
if (-not $Proto) { throw "Could not read PFBuild::NetProtocol; refusing to create an unversioned package." }
Write-Host "==> NetProtocol $Proto  (itch userversion would be 0.1.0-alpha.$Proto)"
if (($Config -eq "Shipping" -or $env:PF_REQUIRE_PROTOCOL_BUMP -eq "1") -and
	-not $AllowSameProtocol -and $Proto -and (Test-Path $ProtoStamp)) {
	$LastProto = (Get-Content $ProtoStamp -Raw).Trim()
	if ($Proto -eq $LastProto) {
		throw "REFUSING TO PACKAGE: NetProtocol is still $Proto, the value already packaged. Bump PFBuild::NetProtocol, or pass -AllowSameProtocol only for an intentional rebuild of the same release."
	}
}

Write-Host "==> BuildCookRun -> $ArchiveDir"
Write-Host "    Config=$Config  (editor must be closed)"

& $RunUAT @UatArgs
if ($LASTEXITCODE -ne 0) { throw "Package failed ($LASTEXITCODE)" }

$ClientDir = Join-Path $ArchiveDir "Windows"
if (-not (Test-Path $ClientDir)) { throw "Package completed but Windows archive is missing: $ClientDir" }

# SECURITY SCRUB (durable fix for the 2026-07-17 token leak): a packaged build run from a writable folder
# writes runtime data into <package>\CombatForge\Saved - INCLUDING a logged-in session token on pre-fix
# builds. That must NEVER be distributed. Also drop debug PDBs (size). The per-user auth-path fix already
# keeps fresh logins out of the package, but this guarantees a stray login or crash dump can't ship.
$SavedDir = Join-Path $ClientDir "CombatForge\Saved"
if (Test-Path $SavedDir) { Remove-Item $SavedDir -Recurse -Force -ErrorAction SilentlyContinue; Write-Host "Scrubbed $SavedDir (never ship runtime login/crash data)" }
Get-ChildItem $ClientDir -Recurse -Filter *.pdb -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

$Forbidden = @()
$Forbidden += Get-ChildItem $ClientDir -Recurse -Directory -Filter Saved -ErrorAction SilentlyContinue
$Forbidden += Get-ChildItem $ClientDir -Recurse -File -Filter Auth.json -ErrorAction SilentlyContinue
$Forbidden += Get-ChildItem $ClientDir -Recurse -File -Filter ServerKey.txt -ErrorAction SilentlyContinue
$Forbidden += Get-ChildItem $ClientDir -Recurse -File -Filter *.pdb -ErrorAction SilentlyContinue
if ($Forbidden.Count -gt 0) {
	$Forbidden | ForEach-Object { Write-Host "FORBIDDEN: $($_.FullName)" }
	throw "Release privacy scrub failed; refusing to mark the package complete."
}

$ConnectSrc = Join-Path $PSScriptRoot "connect.ps1"
if ($Config -eq "Development" -and (Test-Path $ClientDir) -and (Test-Path $ConnectSrc)) {
	Copy-Item $ConnectSrc (Join-Path $ClientDir "connect.ps1") -Force
	# Also copy if nested
	Get-ChildItem $ClientDir -Directory -ErrorAction SilentlyContinue | ForEach-Object {
		Copy-Item $ConnectSrc (Join-Path $_.FullName "connect.ps1") -Force -ErrorAction SilentlyContinue
	}
	Write-Host "Copied connect.ps1 into client package"
}

$ClientExe = Get-ChildItem $ClientDir -Recurse -Filter "CombatForge.exe" -File -ErrorAction SilentlyContinue |
	Sort-Object Length -Descending | Select-Object -First 1
if (-not $ClientExe) { throw "No CombatForge.exe found under $ClientDir" }
$GitSha = (& git -C $ProjectRoot rev-parse HEAD 2>$null)
$Manifest = [ordered]@{
	product = "CombatForge"
	configuration = $Config
	netProtocol = [int]$Proto
	gitCommit = if ($LASTEXITCODE -eq 0) { "$GitSha".Trim() } else { "unknown" }
	createdUtc = [DateTime]::UtcNow.ToString("o")
	executable = $ClientExe.FullName.Substring($ClientDir.Length).TrimStart(
		[IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
	executableSha256 = (Get-FileHash -LiteralPath $ClientExe.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$Manifest | ConvertTo-Json | Set-Content -Path (Join-Path $ClientDir "CombatForge-build.json") -Encoding utf8

# Stamp only after the archive, executable, scrub, and manifest have all passed. A failed build must
# never consume the protocol number and block the retry.
New-Item -ItemType Directory -Force -Path (Split-Path $ProtoStamp) | Out-Null
Set-Content -Path $ProtoStamp -Value $Proto -Encoding ascii

Write-Host "OK: packaged under $ArchiveDir"
Get-ChildItem $ArchiveDir -Recurse -Filter "CombatForge.exe" -ErrorAction SilentlyContinue |
	ForEach-Object { Write-Host "  CLIENT: $($_.FullName)" }

Write-Host ""
Write-Host "Host:   .\Deploy\playtest\run-listen.ps1"
Write-Host "        .\Deploy\playtest\run-server.ps1"
Write-Host "Friends: zip the Windows client folder + connect.ps1"
