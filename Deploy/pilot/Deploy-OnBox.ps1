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

function Stop-OnBoxRestartHosts {
    # Never touch $PID (this deploy). Instances=1 leaves the OLD Deploy-OnBox
    # process inside Start-Server-OnBox's while-loop; its CommandLine is
    # Deploy-OnBox.ps1 and its window title is "CombatForge server #N ...".
    $self = $PID

    $logDir = Join-Path $GameDir "Logs"
    foreach ($name in @("host.pid", "host2.pid", "host3.pid")) {
        $pf = Join-Path $logDir $name
        if (Test-Path $pf) {
            $oldPid = 0
            try { $oldPid = [int]((Get-Content $pf -Raw).Trim()) } catch { $oldPid = 0 }
            if ($oldPid -gt 0 -and $oldPid -ne $self) {
                Stop-Process -Id $oldPid -Force -ErrorAction SilentlyContinue
            }
        }
    }

    $cim = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ProcessId -ne $self -and
            $_.Name -match '^(powershell|pwsh)\.exe$' -and
            $_.CommandLine -and
            $_.CommandLine -match 'Start-Server-OnBox\.ps1|Start-Fleet-OnBox\.ps1|Deploy-OnBox\.ps1'
        }
    foreach ($p in $cim) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }

    Get-Process -Name powershell,pwsh -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $self -and $_.MainWindowTitle -match 'CombatForge server #' } |
        Stop-Process -Force -ErrorAction SilentlyContinue

    Get-Process -Name "CombatForge*" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    $deadline = (Get-Date).AddSeconds(20)
    do {
        Start-Sleep -Seconds 1
        $psLeft = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
            Where-Object {
                $_.ProcessId -ne $self -and
                $_.Name -match '^(powershell|pwsh)\.exe$' -and
                $_.CommandLine -and
                $_.CommandLine -match 'Start-Server-OnBox\.ps1|Start-Fleet-OnBox\.ps1'
            }).Count
        $titleLeft = @(Get-Process -Name powershell,pwsh -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -ne $self -and $_.MainWindowTitle -match 'CombatForge server #' }).Count
        $exeLeft = @(Get-Process -Name "CombatForge*" -ErrorAction SilentlyContinue).Count
    } while (($psLeft + $titleLeft + $exeLeft) -gt 0 -and (Get-Date) -lt $deadline)

    if (($psLeft + $titleLeft + $exeLeft) -gt 0) {
        throw "Old restart-loop host(s) still running (ps=$psLeft title=$titleLeft exe=$exeLeft). Close those windows and re-run."
    }
}

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

# ---- 3. Protect the server key (ProgramData live + tree + InstanceN) ----
# Canonical live key is %ProgramData%\CombatForge\ServerKey.txt (and InstanceN if those
# dirs exist). The tree file next to the launcher is a fallback migrate source only -
# always back it up so a zip stub cannot become the next OnBox migrate source.
Say ""
$BackupRoot = Join-Path $env:TEMP "cf-key-backup"
if (Test-Path $BackupRoot) { Remove-Item $BackupRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $BackupRoot | Out-Null

$PdRoot     = Join-Path $env:ProgramData "CombatForge"
$PdKey      = Join-Path $PdRoot "ServerKey.txt"
$Inst2Key   = Join-Path $PdRoot "Instance2\ServerKey.txt"
$Inst3Key   = Join-Path $PdRoot "Instance3\ServerKey.txt"
$TreeBak    = Join-Path $BackupRoot (Join-Path $GameFolderName "ServerKey.txt")
$PdBak      = Join-Path $BackupRoot "CombatForge\ServerKey.txt"
$Inst2Bak   = Join-Path $BackupRoot "CombatForge\Instance2\ServerKey.txt"
$Inst3Bak   = Join-Path $BackupRoot "CombatForge\Instance3\ServerKey.txt"

function Backup-LiveKey([string]$Live, [string]$Bak) {
    if (Test-Path $Live) {
        New-Item -ItemType Directory -Force -Path (Split-Path $Bak) | Out-Null
        Copy-Item $Live $Bak -Force
        return $true
    }
    return $false
}

$hadAnyKey = $false
if (Backup-LiveKey $KeyFile $TreeBak)  { $hadAnyKey = $true }
if (Backup-LiveKey $PdKey   $PdBak)    { $hadAnyKey = $true }
if (Test-Path (Join-Path $PdRoot "Instance2")) {
    if (Backup-LiveKey $Inst2Key $Inst2Bak) { $hadAnyKey = $true }
}
if (Test-Path (Join-Path $PdRoot "Instance3")) {
    if (Backup-LiveKey $Inst3Key $Inst3Bak) { $hadAnyKey = $true }
}

if ($hadAnyKey) {
    Say "ServerKey.txt found and backed up." Green
} else {
    Say "WARNING: no ServerKey.txt at $KeyFile or $PdKey" Yellow
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

# ---- 5. Stop leftover restart hosts + CombatForge* ----
# Only now, with a verified archive on disk, is it safe to take the box offline.
# Killing CombatForge* alone leaves the parent while ($true) host alive; default
# restart delay is 3 s, so a 3 s sleep is a race the old host often wins.
Say ""
Say "Stopping leftover restart hosts and any running server..." Gray
Stop-OnBoxRestartHosts
Say "  Old hosts are gone." Green

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

function Restore-LiveKey([string]$Bak, [string]$Live) {
    if (Test-Path $Bak) {
        New-Item -ItemType Directory -Force -Path (Split-Path $Live) | Out-Null
        Copy-Item $Bak $Live -Force
        Say "  ServerKey.txt restored from backup." Green
    }
}

if ((Test-Path $KeyFile) -and (Test-Path $TreeBak)) {
    $liveHash = (Get-FileHash $KeyFile -Algorithm SHA256).Hash
    $bakHash  = (Get-FileHash $TreeBak -Algorithm SHA256).Hash
    if ($liveHash -ne $bakHash) {
        Say "  WARNING: zip tried to replace the fleet key at $KeyFile - restoring backed-up bytes." Yellow
    }
}

Restore-LiveKey $TreeBak  $KeyFile
Restore-LiveKey $PdBak    $PdKey
Restore-LiveKey $Inst2Bak $Inst2Key
Restore-LiveKey $Inst3Bak $Inst3Key

# First-time migrate during deploy: tree backup exists, ProgramData did not.
if (-not (Test-Path $PdBak) -and (Test-Path $TreeBak)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $PdKey) | Out-Null
    Copy-Item $TreeBak $PdKey -Force
    Say "  ServerKey.txt restored from backup." Green
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
