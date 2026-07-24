# CombatForge - headless dedicated server for a cloud/VPS box (e.g. Vultr Windows Server).
#
# Runs the PACKAGED client exe headless as the fleet dedicated server, pointed at the LIVE backend
# (api.playcombatforge.com). Reads the server key from ServerKey.txt sitting next to this script.
# On boot it registers with the directory + heartbeats, so it shows up in the in-game SERVERS list
# and QUICK PLAY, and it can mint XP via HMAC-signed match reports (fleet hat).
#
# SETUP ON THE BOX (once):
#   1) Extract the CombatForge Windows build into this folder (so CombatForge.exe is here or in a subfolder).
#   2) Copy ServerKey.txt (the secret) into this same folder.
#   3) Right-click this file -> "Run with PowerShell"  (the FIRST run should be as Administrator so the
#      firewall rule can be added; after that a normal run is fine).
#
# Stop: close this window or press Ctrl+C.

param(
  [int]$Port = 7777,
  [string]$Map = "L_Graybox",
  # Fleet slot number (1..N). Instance 1 keeps the legacy data dir + log name so an existing
  # single-server box upgrades in place (built maps + unsent XP reports survive). Instances 2+
  # get their own data dir (maps/PendingReports/JoinCode are per-match-instance state).
  [int]$InstanceIndex = 1
)

try { $Host.UI.RawUI.WindowTitle = "CombatForge server #$InstanceIndex (port $Port)" } catch {}

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

# --- Locate the REAL packaged game exe (NOT the tiny root bootstrap launcher) ---
# The root Windows\CombatForge.exe is a ~166 KB bootstrapper that spawns the real 318 MB exe under
# CombatForge\Binaries\Win64 and then RETURNS IMMEDIATELY. If we launch that, "& $Exe" does not block,
# the restart loop below fires every 3 s, and real server processes pile up fighting over the port.
# So we deliberately target the nested Binaries\Win64\CombatForge.exe, which blocks until true exit.
$Exe = $null
foreach ($rel in @("CombatForge\Binaries\Win64\CombatForge.exe", "Windows\CombatForge\Binaries\Win64\CombatForge.exe", "Binaries\Win64\CombatForge.exe")) {
  $p = Join-Path $Here $rel
  if (Test-Path $p) { $Exe = $p; break }
}
if (-not $Exe) {
  # Prefer the largest CombatForge.exe found (the real game exe, not the tiny launcher).
  $f = Get-ChildItem $Here -Recurse -Filter CombatForge.exe -ErrorAction SilentlyContinue |
       Sort-Object Length -Descending | Select-Object -First 1
  if ($f) { $Exe = $f.FullName }
}
if (-not $Exe) { throw "CombatForge.exe not found under $Here - extract the Windows build here first." }

# --- Server key (secret) - read from ServerKey.txt next to this script ---
$KeyFile = Join-Path $Here "ServerKey.txt"
if (-not (Test-Path $KeyFile)) { throw "ServerKey.txt not found next to this script - copy it here." }
$Key = (Get-Content $KeyFile -Raw).Trim()
if ($Key.Length -lt 32) { throw "ServerKey.txt looks wrong (too short)." }

# --- Prerequisite: Microsoft Visual C++ 2015-2022 Redistributable (x64) ---
# A fresh Windows Server lacks this and the UE exe refuses to start ("component(s) required").
# Detect via the VC runtime registry key (with a DLL fallback) and silently install if missing.
$vcInstalled = $false
try {
  $rk = "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"
  if ((Test-Path $rk) -and ((Get-ItemProperty $rk -Name Installed -ErrorAction SilentlyContinue).Installed -eq 1)) { $vcInstalled = $true }
} catch {}
if (-not $vcInstalled -and (Test-Path "$env:SystemRoot\System32\vcruntime140_1.dll")) { $vcInstalled = $true }
if (-not $vcInstalled) {
  Write-Host "Microsoft VC++ 2015-2022 (x64) runtime not found - installing (the game requires it)..." -ForegroundColor Yellow
  $vc = Join-Path $env:TEMP "vc_redist.x64.exe"
  try {
    & curl.exe -L -s -o $vc "https://aka.ms/vs/17/release/vc_redist.x64.exe"
    Start-Process -FilePath $vc -ArgumentList "/install","/quiet","/norestart" -Wait
    Write-Host "VC++ runtime installed." -ForegroundColor Green
  } catch {
    Write-Host "Auto-install failed. Get it from https://aka.ms/vs/17/release/vc_redist.x64.exe then re-run." -ForegroundColor Red
  }
}

# --- Firewall: open UDP $Port inbound so remote players can reach the server ---
$rule = "CombatForge Server $Port"
if (-not (Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue)) {
  try {
    New-NetFirewallRule -DisplayName $rule -Direction Inbound -Action Allow `
      -Protocol UDP -LocalPort $Port -Profile Any | Out-Null
    Write-Host "Opened Windows Firewall for UDP $Port." -ForegroundColor Green
  } catch {
    Write-Host "NOTE: could not add the firewall rule (re-run as Administrator once if joins fail)." -ForegroundColor Yellow
  }
}

# The map URL MUST carry ?listen so the server actually opens the port (a plain -server boots a
# non-listening standalone). -nullrhi/-nosound = headless. -PFServerKey passes the fleet key; the
# API base defaults to https://api.playcombatforge.com (no override needed).
# Persistent, verbose log for monitoring. -ABSLOG pins the file to a known path (next to this script) so
# it's easy to tail; UE renames the previous run's log to a -backup- file on each restart. CombatForgeLog
# runs Verbose (gameplay detail) while net stays at Log so the file doesn't drown in per-packet spam.
$LogDir = Join-Path $Here "Logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogName = if ($InstanceIndex -le 1) { "server.log" } else { "server$InstanceIndex.log" }
$LogFile = Join-Path $LogDir $LogName

# --- PERSISTENT data dir (built maps + un-sent XP reports) - MUST live OUTSIDE the extracted build ---
# The build runs with -NOHOMEDIR, so UE's UserSettingsDir() (where maps + PendingReports would otherwise land)
# resolves INSIDE this folder and is wiped every time you re-extract a new build over it - which silently
# dropped built maps AND un-minted XP on every redeploy (#8/#9). Passing -ArenaDir pins them to ProgramData
# (survives re-extraction). The game derives PendingReports/JoinCode/ServerKey.txt from this dir's parent too.
$DataDir = Join-Path $env:ProgramData "CombatForge"
if ($InstanceIndex -gt 1) { $DataDir = Join-Path $DataDir "Instance$InstanceIndex" }
$ArenaDir = Join-Path $DataDir "Arenas"
New-Item -ItemType Directory -Force -Path $ArenaDir | Out-Null

$mapUrl = "$Map`?listen"
# CombatForge.exe is a GUI-subsystem app, so PowerShell's "& $exe" call operator returns INSTANTLY without
# waiting for it - which made the restart loop below spawn a new server every 3 s (stacked processes fighting
# over the port). Launch via Start-Process -PassThru + WaitForExit so we block until the server truly exits.
# ArgumentList is ONE string on purpose: Start-Process -ArgumentList as an ARRAY drops the quotes around
# space-containing args (e.g. -LogCmds="CombatForgeLog Verbose, LogNet Log"), corrupting them.
$cmdLine = @(
    $mapUrl, "-server", "-nullrhi", "-nosound", "-log", "-port=$Port", "-NOHOMEDIR", "-PFServerKey=$Key",
    "`"-ArenaDir=$ArenaDir`"",
    "`"-ABSLOG=$LogFile`"", "`"-LogCmds=CombatForgeLog Verbose, LogNet Log`""
) -join " "

Write-Host ""
Write-Host "=== CombatForge dedicated server ===" -ForegroundColor Cyan
Write-Host "  Exe     : $Exe"
Write-Host "  Backend : https://api.playcombatforge.com (LIVE)"
Write-Host "  Port    : $Port/udp"
Write-Host "  Map     : $Map"
Write-Host "  Data    : $DataDir  (persistent: maps + un-sent XP survive redeploys)"
Write-Host "  Log     : $LogFile"
Write-Host "            (live-tail in another window:  Get-Content '$LogFile' -Wait -Tail 50)" -ForegroundColor DarkGray
Write-Host "  Watch the log for:  Backend: fleet registered (port $Port)" -ForegroundColor Cyan
Write-Host "  Then it appears in the game's SERVERS list + QUICK PLAY. Ctrl+C or close to stop."
Write-Host ""

# Restart-on-exit loop (crude Restart=always) with crash-loop backoff (issue #21 D9/D8): a server
# that dies inside 60s doubles the delay (3s -> 300s cap) instead of hammering restarts; any run
# longer than 60s resets the delay to 3s.
$RestartDelay = 3
while ($true) {
  Write-Host ("[{0}] launching server..." -f (Get-Date -Format "HH:mm:ss")) -ForegroundColor Green
  $LaunchedAt = Get-Date
  $proc = Start-Process -FilePath $Exe -ArgumentList $cmdLine -PassThru
  if ($proc) { $proc.WaitForExit() }   # BLOCKS until the GUI server process actually exits (not the & bug)
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
