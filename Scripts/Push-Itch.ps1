# Copyright (c) 2026 Tom Chapman. All rights reserved.
#
# Push a packaged Windows build to itch.io.
#
# docs/itch-deploy.md has described this script since alpha-11 but it was never actually created,
# and the version it described pointed at Packaged\Windows - which does not exist on the build
# machine. alpha-15, alpha-16 and alpha-17 were all cooked by Deploy\playtest\package-playtest.ps1
# into Packaged\Playtest\Windows, so that is the default here.
#
# The alpha number lives in exactly ONE place - PFBuild::NetProtocol in CombatForge.h - because
# that constant is what the join handshake gates on. Typing --userversion by hand is how the itch
# page and the wire protocol drift apart: the page says alpha.17 while the binary still speaks 16,
# and players get "update needed" on a build they just downloaded.
#
#   .\Scripts\Push-Itch.ps1                 # dry run: every check, no push
#   .\Scripts\Push-Itch.ps1 -Push           # actually push
#
param(
    [string]$Channel  = "thathorseslayer/combatforge:windows-alpha",
    [string]$BuildDir = "",
    [string]$Butler   = "",
    [switch]$Push
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $ProjectRoot "Packaged\Playtest\Windows" }

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

# --- 2. The build must exist and be scrubbed ---
if (-not (Test-Path $BuildDir)) {
    throw "No packaged build at $BuildDir - run Deploy\playtest\package-playtest.ps1 first."
}
$SavedDir = Join-Path $BuildDir "CombatForge\Saved"
if (Test-Path $SavedDir) {
    throw "$SavedDir still present - the privacy scrub did not run, or a smoke boot re-created it. " +
          "This is how a login session token shipped to every download in alpha-4/5. Re-scrub before pushing."
}
$Pdbs = @(Get-ChildItem $BuildDir -Recurse -Filter *.pdb -ErrorAction SilentlyContinue)
if ($Pdbs.Count -gt 0) {
    throw "$($Pdbs.Count) .pdb file(s) still in the package - re-run the scrub."
}
$Exe = Join-Path $BuildDir "CombatForge.exe"
if (-not (Test-Path $Exe)) { throw "No CombatForge.exe at $BuildDir - is this the right build dir?" }
$SizeGB = [math]::Round((Get-ChildItem $BuildDir -Recurse -File |
    Measure-Object -Property Length -Sum).Sum / 1GB, 2)
Write-Host "Build:   $BuildDir  ($SizeGB GB)" -ForegroundColor Cyan
Write-Host "Channel: $Channel" -ForegroundColor Cyan

# --- 3. Refuse to re-push a version the channel already has ---
# That always means the header bump was skipped, and pushing anyway ships a binary that cannot
# talk to the live servers.
$Status = (& $Butler status $Channel) -join "`n"
Write-Host ""
Write-Host $Status
if ($Status -match [regex]::Escape($UserVersion)) {
    throw "$UserVersion is ALREADY on $Channel. Bump PFBuild::NetProtocol in CombatForge.h first."
}

# --- 4. Push (only with -Push) ---
if (-not $Push) {
    Write-Host ""
    Write-Host "DRY RUN - all checks passed. Re-run with -Push to actually upload." -ForegroundColor Yellow
    Write-Host "REMINDER: the dedicated server must be redeployed at the SAME protocol ($Proto)," -ForegroundColor Yellow
    Write-Host "          or the fleet and the new client cannot see each other." -ForegroundColor Yellow
    exit 0
}

Write-Host ""
Write-Host "Pushing..." -ForegroundColor Green
& $Butler push $BuildDir $Channel --userversion $UserVersion
if ($LASTEXITCODE -ne 0) { throw "butler push failed ($LASTEXITCODE)" }
& $Butler status $Channel
Write-Host ""
Write-Host "Client is live at $UserVersion. The SERVER still needs redeploying at protocol $Proto." -ForegroundColor Yellow
