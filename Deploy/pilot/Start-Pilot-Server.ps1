# CombatForge - free home pilot server (Path C).
#
# Runs the already-compiled Development build as a headless dedicated server on THIS PC, pointed
# at the LIVE backend (api.playcombatforge.com). The server key is read automatically from
# Saved\CombatForge\ServerKey.txt (already placed for you) - nothing secret is in this script.
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

if (-not (Test-Path $Exe))     { throw "Missing $Exe - build the game first (open the project and compile)." }
if (-not (Test-Path $KeyFile)) { throw "Missing $KeyFile - the server key. Ask Claude to re-place it." }

# Open the Windows Firewall for this port, best-effort. UDP carries gameplay; TCP matches what
# Deploy/playtest/_common.ps1 opens, so no join path is blocked by a protocol the pilot skipped.
# Each protocol is checked on its own, so a box that once got only UDP still picks up TCP. (P2-D4)
$ruleUdp = "CombatForge Pilot $Port"
$ruleTcp = "CombatForge Pilot $Port TCP"
foreach ($fw in @(@{ Name = $ruleUdp; Proto = "UDP" }, @{ Name = $ruleTcp; Proto = "TCP" })) {
  if (Get-NetFirewallRule -DisplayName $fw.Name -ErrorAction SilentlyContinue) { continue }
  try {
    New-NetFirewallRule -DisplayName $fw.Name -Direction Inbound -Action Allow `
      -Protocol $fw.Proto -LocalPort $Port -Profile Any | Out-Null
    Write-Host "Opened Windows Firewall for $($fw.Proto) $Port." -ForegroundColor Green
  } catch {
    Write-Host "NOTE: couldn't add the $($fw.Proto) firewall rule automatically (run as Admin once if joins fail)." -ForegroundColor Yellow
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
# P2-D1: CombatForge.exe is a GUI-subsystem app, so "& $Exe" returns INSTANTLY without waiting -
# this loop then launched a new server every 3s and they piled up fighting over the port (the
# exact bug Start-Server-OnBox.ps1 fixed). Same cure: Start-Process -PassThru + WaitForExit,
# single argument string (an ArgumentList ARRAY drops quotes on space-containing args), plus the
# OnBox crash-loop backoff so a boot-crash cannot hammer restarts.
$cmdLine = ($args | ForEach-Object { if ("$_" -match '\s') { '"' + $_ + '"' } else { "$_" } }) -join ' '
$RestartDelay = 3
while ($true) {
  Write-Host ("[{0}] launching server..." -f (Get-Date -Format "HH:mm:ss")) -ForegroundColor Green
  $LaunchedAt = Get-Date
  $proc = Start-Process -FilePath $Exe -ArgumentList $cmdLine -PassThru
  if ($proc) { $proc.WaitForExit() }
  $code = if ($proc) { $proc.ExitCode } else { "?" }
  $RanSecs = ((Get-Date) - $LaunchedAt).TotalSeconds
  if ($RanSecs -lt 60) {
    $RestartDelay = [Math]::Min($RestartDelay * 2, 300)
    Write-Host ("[{0}] server exited after only {1:n0}s (code {2}) - CRASH LOOP? backing off {3}s. Ctrl+C to stop." -f (Get-Date -Format "HH:mm:ss"), $RanSecs, $code, $RestartDelay) -ForegroundColor Red
  } else {
    $RestartDelay = 3
    Write-Host ("[{0}] server exited (code {1}) - restarting in {2}s. Ctrl+C to stop." -f (Get-Date -Format "HH:mm:ss"), $code, $RestartDelay) -ForegroundColor Yellow
  }
  Start-Sleep -Seconds $RestartDelay
}
