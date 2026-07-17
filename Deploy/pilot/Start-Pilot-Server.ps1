# CombatForge — free home pilot server (Path C).
#
# Runs the already-compiled Development build as a headless dedicated server on THIS PC, pointed
# at the LIVE backend (api.playcombatforge.com). The server key is read automatically from
# Saved\CombatForge\ServerKey.txt (already placed for you) — nothing secret is in this script.
#
# It registers with the live directory + heartbeats, so it shows up in the in-game SERVERS list
# and QUICK PLAY. Auto-restarts if it ever exits. Close this window (or Ctrl+C) to stop it.
#
# USAGE:  right-click -> Run with PowerShell   (or:  powershell -ExecutionPolicy Bypass -File Deploy\pilot\Start-Pilot-Server.ps1)

param(
  [int]$Port = 7777,
  [string]$Map = "L_Graybox"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$UProject = Join-Path $ProjectRoot "CombatForge.uproject"
$Exe = Join-Path $ProjectRoot "Binaries\Win64\CombatForge.exe"
$KeyFile = Join-Path $ProjectRoot "Saved\CombatForge\ServerKey.txt"

if (-not (Test-Path $Exe))     { throw "Missing $Exe — build the game first (open the project and compile)." }
if (-not (Test-Path $KeyFile)) { throw "Missing $KeyFile — the server key. Ask Claude to re-place it." }

# Open the Windows Firewall for this port (both UDP for gameplay + the process), best-effort.
$ruleName = "CombatForge Pilot $Port"
if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
  try {
    New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Action Allow `
      -Protocol UDP -LocalPort $Port -Profile Any | Out-Null
    Write-Host "Opened Windows Firewall for UDP $Port." -ForegroundColor Green
  } catch {
    Write-Host "NOTE: couldn't add a firewall rule automatically (run as Admin once if joins fail)." -ForegroundColor Yellow
  }
}

# The map URL MUST carry ?listen so the server actually opens the port (a plain -server boots a
# non-listening standalone). -nullrhi/-nosound = headless. The server KEY + API base are picked up
# automatically (ServerKey.txt + the built-in api.playcombatforge.com default).
$mapUrl = "$Map`?listen"
$args = @($mapUrl, "-server", "-nullrhi", "-nosound", "-log", "-port=$Port", "-NOHOMEDIR",
          "-project=$UProject")

Write-Host ""
Write-Host "=== CombatForge home pilot server ===" -ForegroundColor Cyan
Write-Host "  Backend : https://api.playcombatforge.com (LIVE)"
Write-Host "  Port    : $Port/udp"
Write-Host "  Map     : $Map"
Write-Host "  Watch the log for:  Backend: fleet registered (port $Port)" -ForegroundColor Cyan
Write-Host "  Then it appears in the game's SERVERS list. Ctrl+C or close this window to stop."
Write-Host ""

# Restart-on-exit loop (the crude equivalent of systemd Restart=always).
while ($true) {
  Write-Host ("[{0}] launching server..." -f (Get-Date -Format "HH:mm:ss")) -ForegroundColor Green
  & $Exe @args
  Write-Host ("[{0}] server exited (code {1}) — restarting in 3s. Ctrl+C to stop." -f (Get-Date -Format "HH:mm:ss"), $LASTEXITCODE) -ForegroundColor Yellow
  Start-Sleep -Seconds 3
}
