# Copyright (c) 2026 Tom Chapman. All rights reserved.
#
# Validate and push a packaged Windows build to itch.io.
#
# docs/itch-deploy.md has described this script since alpha-11 but it was never actually created,
# and the version it described pointed at Packaged\Windows - which does not exist on the build
# machine. alpha-15, alpha-16 and alpha-17 were all cooked by Deploy\playtest\package-playtest.ps1
# into Packaged\Playtest\Windows. Public Alpha releases now use the hardened Shipping archive at
# Packaged\Release\Windows; Development packages require an explicit -AllowDevelopment override.
#
# The alpha number lives in exactly ONE place - PFBuild::NetProtocol in CombatForge.h - because
# that constant is what the join handshake gates on. Typing --userversion by hand is how the itch
# page and the wire protocol drift apart: the page says alpha.17 while the binary still speaks 16,
# and LAN peers get an opaque version-mismatch failure on a build they just downloaded.
#
#   .\Scripts\Push-Itch.ps1                 # Shipping dry run: every check, no push
#   .\Scripts\Push-Itch.ps1 -Push           # actually push the verified Shipping artifact
#
param(
    [string]$Channel  = "thathorseslayer/combatforge:windows-alpha",
    [string]$BuildDir = "",
	[string]$Butler   = "",
	[switch]$AllowDevelopment,
    [switch]$Push
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $ProjectRoot "Packaged\Release\Windows" }

# butler: the itch app's copy first, then the standalone download.
if (-not $Butler) {
    $Candidates = @(
        (Join-Path $env:APPDATA "itch\broth\butler\versions\15.29.0\butler.exe"),
        "D:\projects\butler-windows-amd64\butler.exe"
    )
    $Butler = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Butler -or -not (Test-Path $Butler)) {
    throw "butler.exe not found. Pass -Butler <path>."
}

# --- 1. Version comes from the header, never from a typed argument ---
$Header = Join-Path $ProjectRoot "Source\CombatForge\CombatForge.h"
$m = Select-String -Path $Header -Pattern 'constexpr\s+int32\s+NetProtocol\s*=\s*(\d+)\s*;'
if (-not $m) { throw "Could not read NetProtocol from $Header - did the declaration change shape?" }
$Proto = [int]$m.Matches[0].Groups[1].Value
$UserVersion = "0.1.0-alpha.$Proto"
Write-Host "NetProtocol $Proto  ->  --userversion $UserVersion" -ForegroundColor Cyan

# --- 2. The build must exist, be a validated Shipping archive, and remain scrubbed ---
if (-not (Test-Path $BuildDir)) {
	throw "No packaged build at $BuildDir - run Deploy\playtest\package-playtest.ps1 -Config Shipping first."
}
$BuildDir = [IO.Path]::GetFullPath((Resolve-Path $BuildDir).Path)
$SavedDir = Join-Path $BuildDir "CombatForge\Saved"
if (Test-Path $SavedDir) {
    throw "$SavedDir still present - the privacy scrub did not run, or a smoke boot re-created it. " +
          "This is how a login session token shipped to every download in alpha-4/5. Re-scrub before pushing."
}
$Pdbs = @(Get-ChildItem $BuildDir -Recurse -Filter *.pdb -ErrorAction SilentlyContinue)
if ($Pdbs.Count -gt 0) {
	throw "$($Pdbs.Count) .pdb file(s) still in the package - re-run the scrub."
}
$ForbiddenRuntime = @()
$ForbiddenRuntime += Get-ChildItem $BuildDir -Recurse -Directory -Filter Saved -ErrorAction SilentlyContinue
$ForbiddenRuntime += Get-ChildItem $BuildDir -Recurse -File -Filter Auth.json -ErrorAction SilentlyContinue
$ForbiddenRuntime += Get-ChildItem $BuildDir -Recurse -File -Filter ServerKey.txt -ErrorAction SilentlyContinue
if ($ForbiddenRuntime.Count -gt 0) {
	$ForbiddenRuntime | ForEach-Object { Write-Host "FORBIDDEN: $($_.FullName)" }
	throw "Runtime data or credentials remain in the artifact; refusing upload."
}
$ManifestPath = Join-Path $BuildDir "CombatForge-build.json"
if (-not (Test-Path $ManifestPath)) {
	throw "No CombatForge-build.json at $BuildDir - this archive did not complete the Shipping gate."
}
try {
	$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
} catch {
	throw "CombatForge-build.json is invalid: $($_.Exception.Message)"
}
if (-not $AllowDevelopment -and "$($Manifest.configuration)" -ne "Shipping") {
	throw "Refusing to publish a $($Manifest.configuration) package. Cook Shipping, or pass -AllowDevelopment intentionally."
}
if ([int]$Manifest.netProtocol -ne $Proto) {
	throw "Manifest protocol $($Manifest.netProtocol) does not match source protocol $Proto. Repackage from this checkout."
}
$SourceGit = (& git -C $ProjectRoot rev-parse HEAD)
if ($LASTEXITCODE -ne 0) { throw "Could not read the source Git commit." }
$SourceGit = "$SourceGit".Trim()
if ("$($Manifest.gitCommit)" -ne $SourceGit) {
	throw "Manifest commit $($Manifest.gitCommit) does not match checkout $SourceGit. Repackage or switch back to the packaged commit."
}
$ExeRelative = "$($Manifest.executable)"
if (-not $ExeRelative -or [IO.Path]::IsPathRooted($ExeRelative)) {
	throw "Manifest executable must be a relative path (got '$ExeRelative')."
}
$Exe = [IO.Path]::GetFullPath((Join-Path $BuildDir $ExeRelative))
if (-not $Exe.StartsWith($BuildDir + [IO.Path]::DirectorySeparatorChar,
	[StringComparison]::OrdinalIgnoreCase)) {
	throw "Manifest executable escapes the package root: $ExeRelative"
}
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
	throw "Manifest executable is missing: $Exe"
}
$ActualExeHash = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ActualExeHash -ne "$($Manifest.executableSha256)".ToLowerInvariant()) {
	throw "Executable SHA-256 does not match CombatForge-build.json; refusing a partial/stale upload."
}
$SizeGB = [math]::Round((Get-ChildItem $BuildDir -Recurse -File |
    Measure-Object -Property Length -Sum).Sum / 1GB, 2)
Write-Host "Build:   $BuildDir  ($SizeGB GB)" -ForegroundColor Cyan
Write-Host "Channel: $Channel" -ForegroundColor Cyan

# --- 3. Refuse to re-push a version the channel already has ---
# That means the public userversion would be ambiguous. Bump NetProtocol for the next public build.
$Status = (& $Butler status $Channel) -join "`n"
Write-Host ""
Write-Host $Status
# (?!\d): a bare substring test makes 0.1.0-alpha.2 "already pushed" the moment alpha.20 exists.
if ($Status -match ([regex]::Escape($UserVersion) + '(?!\d)')) {
    throw "$UserVersion is ALREADY on $Channel. Bump PFBuild::NetProtocol in CombatForge.h first."
}

# --- 4. Push (only with -Push) ---
if (-not $Push) {
    Write-Host ""
    Write-Host "DRY RUN - all checks passed. Re-run with -Push to actually upload." -ForegroundColor Yellow
	Write-Host "LAN-ONLY ALPHA: no official fleet deployment is required for this release." -ForegroundColor Yellow
	Write-Host "                  LAN peers must run this same alpha.$Proto build." -ForegroundColor Yellow
    exit 0
}

Write-Host ""
Write-Host "Pushing..." -ForegroundColor Green
& $Butler push $BuildDir $Channel --userversion $UserVersion
if ($LASTEXITCODE -ne 0) { throw "butler push failed ($LASTEXITCODE)" }
& $Butler status $Channel
Write-Host ""
Write-Host "Windows LAN-only Alpha is live at $UserVersion." -ForegroundColor Green
