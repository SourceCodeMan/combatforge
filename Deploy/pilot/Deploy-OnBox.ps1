# CombatForge - one-shot deploy ON THE BOX.
#
# Keep this file on the server (Desktop is fine). Every future deploy is then:
#   1. On the PC: start the file server + cloudflared tunnel
#   2. On the box: run this script, paste the cloudflare words, walk away
#
# It stops the running server, downloads + extracts the new build, refreshes the launcher,
# and restarts - while protecting ServerKey.txt (without it the server cannot register and
# XP minting silently stops).
#
# NOTE: this file is deliberately ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI unless
# there is a BOM, so smart quotes / em-dashes turn into mojibake and break parsing.

param(
    [string]$Tunnel = "",                                   # full URL or just the random words
    [string]$Root   = "C:\Users\Administrator\Desktop",     # folder that CONTAINS the game folder
    [string]$GameFolderName = "Windows",                    # the extracted build folder
    [string]$ZipName = "",                                  # blank = auto-discover from the tunnel
    [int]$Instances = 1                                     # >1 = launch a fleet (see Start-Fleet-OnBox.ps1)
)

$ErrorActionPreference = "Stop"
$GameDir = Join-Path $Root $GameFolderName
$KeyFile = Join-Path $GameDir "ServerKey.txt"

function Say($msg, $color = "Gray") { Write-Host $msg -ForegroundColor $color }

Say ""
Say "=== CombatForge server deploy ===" Cyan
Say ""

# ---- 1. Tunnel address ----
if (-not $Tunnel) {
    Say "Paste the cloudflared address from the PC window." Yellow
    Say "Full URL or just the words both work (e.g. brave-lions-run-fast)." DarkGray
    $Tunnel = Read-Host "Tunnel"
}
$Tunnel = $Tunnel.Trim().Trim('"').Trim("'")
if (-not $Tunnel) { throw "No tunnel address given." }
if ($Tunnel -notmatch '^https?://') {
    if ($Tunnel -notmatch '\.trycloudflare\.com') { $Tunnel = "$Tunnel.trycloudflare.com" }
    $Tunnel = "https://$Tunnel"
}
$Tunnel = $Tunnel.TrimEnd('/')
Say "Using: $Tunnel" Green

# ---- 2. Reachability check (fail fast, before a 3.8 GB download) ----
Say ""
Say "Checking the tunnel is reachable..." Gray
try {
    $probe = Invoke-WebRequest -Uri "$Tunnel/Start-Server-OnBox.ps1" -Method Head -TimeoutSec 20 -UseBasicParsing
    if ($probe.StatusCode -ne 200) { throw "HTTP $($probe.StatusCode)" }
    Say "  Tunnel OK." Green
} catch {
    Say "  Could not reach the tunnel: $($_.Exception.Message)" Red
    Say "  Check that BOTH windows are still open on the PC (python http.server AND cloudflared)," Yellow
    Say "  and that you pasted the address cloudflared actually printed." Yellow
    throw "Aborting before download."
}

# ---- 2b. Pick the build zip ----
# This used to default to a hardcoded "CombatForge-alpha8.zip". Every new build gets a new name, so the
# stale default silently fetched the WRONG (or a missing) file - and because the running server is stopped
# in step 4 BEFORE the download, a failure here left the box with no server at all while the directory still
# advertised the old version. Auto-discover instead, so the script always deploys whatever is being served.
if (-not $ZipName) {
    Say ""
    Say "Looking up the build file on the tunnel..." Gray
    $index = $null
    try { $index = Invoke-WebRequest -Uri "$Tunnel/" -TimeoutSec 20 -UseBasicParsing } catch { }
    if ($null -eq $index) {
        throw "Could not list $Tunnel - re-run with -ZipName <file> if there is no directory listing."
    }
    $found = @([regex]::Matches($index.Content, 'CombatForge[-A-Za-z0-9._]*\.zip') |
               ForEach-Object { $_.Value } | Sort-Object -Unique)
    if ($found.Count -eq 0) {
        throw "No CombatForge-*.zip is being served at $Tunnel - check the serve folder on the PC."
    }
    if ($found.Count -gt 1) {
        Say "  More than one build zip is being served:" Yellow
        $found | ForEach-Object { Say "    $_" Yellow }
        throw "Serve exactly one zip, or re-run with -ZipName <file>."
    }
    $ZipName = $found[0]
    Say "  Found: $ZipName" Green
}

# ---- 3. Protect the server key ----
Say ""
$KeyBackup = $null
if (Test-Path $KeyFile) {
    $KeyBackup = Join-Path $env:TEMP "ServerKey.backup.txt"
    Copy-Item $KeyFile $KeyBackup -Force
    Say "ServerKey.txt found and backed up." Green
} else {
    Say "WARNING: no ServerKey.txt at $KeyFile" Yellow
    Say "The server will still run, but it cannot register with the directory or grant XP." Yellow
    $go = Read-Host "Continue anyway? (y/N)"
    if ($go -ne "y") { throw "Stopped so you can put ServerKey.txt in place first." }
}

# ---- 4. Download ----
# Downloading BEFORE stopping the server: a mid-download failure used to leave the box offline
# until someone recovered it by hand, because the stop came first. The tunnel HEAD probe earlier
# only proves the URL answers, not that the whole zip arrives. Land it to a .partial name, prove
# it, and only then take the live server down. (P2-D3)
Set-Location $Root
$Zip     = Join-Path $Root "alpha-latest.zip"
$ZipPart = Join-Path $Root "alpha-latest.partial.zip"
if (Test-Path $ZipPart) { Remove-Item $ZipPart -Force }

Say ""
Say "Downloading the build (~3.8 GB - this is the slow part)..." Cyan
& curl.exe -L --fail --progress-bar -o $ZipPart "$Tunnel/$ZipName"
if ($LASTEXITCODE -ne 0) {
    if (Test-Path $ZipPart) { Remove-Item $ZipPart -Force }
    throw "Download failed (curl exit $LASTEXITCODE). The running server was NOT touched."
}

# Size floor: a truncated transfer that still exits 0 (no Content-Length) shows up here. A real
# package is ~3.8 GB, so anything under 1 GB is a proxy error page or a half-written file.
$partBytes = (Get-Item $ZipPart).Length
if ($partBytes -lt 1GB) {
    Remove-Item $ZipPart -Force
    throw ("Download looks truncated ({0:N0} bytes). The running server was NOT touched." -f $partBytes)
}
if (Test-Path $Zip) { Remove-Item $Zip -Force }
Move-Item $ZipPart $Zip -Force
$sizeGB = [math]::Round((Get-Item $Zip).Length / 1GB, 2)
Say "  Downloaded $sizeGB GB." Green

# ---- 5. Stop the running server ----
# Only now, with a verified archive on disk, is it safe to take the box offline.
Say ""
Say "Stopping any running server..." Gray
$procs = Get-Process -Name "CombatForge*" -ErrorAction SilentlyContinue
if ($procs) {
    $procs | Stop-Process -Force
    Start-Sleep -Seconds 3
    Say "  Stopped $($procs.Count) process(es)." Green
} else {
    Say "  Nothing was running." Gray
}

# ---- 6. Extract ----
# The archive root folder is "Windows", so extracting HERE (the parent) merges into the existing
# game folder. Extracting from INSIDE it would nest one level too deep and the launcher would
# find nothing.
Say ""
Say "Extracting..." Cyan
& tar.exe -xf $Zip
if ($LASTEXITCODE -ne 0) { throw "Extract failed (tar exit $LASTEXITCODE)." }
Say "  Extracted." Green

# ---- 7. Refresh the launcher(s) ----
Say ""
Say "Updating the launcher script..." Gray
& curl.exe -L --fail -s -o (Join-Path $GameDir "Start-Server-OnBox.ps1") "$Tunnel/Start-Server-OnBox.ps1"
if ($LASTEXITCODE -ne 0) {
    Say "  WARNING: launcher download failed; keeping the existing one." Yellow
} else {
    Say "  Launcher updated." Green
}
# Fleet launcher rides along when the serve folder has it (optional - single-server boxes skip it).
& curl.exe -L --fail -s -o (Join-Path $GameDir "Start-Fleet-OnBox.ps1") "$Tunnel/Start-Fleet-OnBox.ps1"
if ($LASTEXITCODE -ne 0) {
    Remove-Item (Join-Path $GameDir "Start-Fleet-OnBox.ps1") -Force -ErrorAction SilentlyContinue
    Say "  (No fleet launcher in the serve folder - single-instance only.)" DarkGray
} else {
    Say "  Fleet launcher updated." Green
}

# ---- 8. Verify ----
Say ""
Say "Verifying..." Gray
$RealExe = Join-Path $GameDir "CombatForge\Binaries\Win64\CombatForge.exe"
if (-not (Test-Path $RealExe)) {
    throw "Game exe missing at $RealExe - the extract landed in the wrong place."
}
Say "  Game exe present." Green

if ($KeyBackup -and -not (Test-Path $KeyFile)) {
    Copy-Item $KeyBackup $KeyFile -Force
    Say "  ServerKey.txt was missing after extract - restored from backup." Yellow
} elseif (Test-Path $KeyFile) {
    Say "  ServerKey.txt intact." Green
}

Remove-Item $Zip -Force -ErrorAction SilentlyContinue

# ---- 9. Launch ----
Say ""
Say "=== Starting the server ===" Cyan
Say "Watch for:  Backend: fleet registered (port 7777)" Yellow
Say "You can close the python and cloudflared windows on the PC now." DarkGray
Say ""
Set-Location $GameDir
$FleetScript = Join-Path $GameDir "Start-Fleet-OnBox.ps1"
if ($Instances -gt 1 -and (Test-Path $FleetScript)) {
    & $FleetScript -Instances $Instances
} else {
    if ($Instances -gt 1) { Say "  Fleet launcher missing - starting a single instance." Yellow }
    & (Join-Path $GameDir "Start-Server-OnBox.ps1")
}
