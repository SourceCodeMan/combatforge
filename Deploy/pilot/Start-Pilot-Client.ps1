# CombatForge - pilot CLIENT (a normal windowed game to test-join your own pilot server).
#
# Launches the already-compiled game as a standalone client (NOT the editor - a real -game window,
# so the ONLINE server-browser "join" works cleanly). Use this to log in, browse, and join the
# server you started with Start-Pilot-Server.ps1.
#
# USAGE:  right-click -> Run with PowerShell

$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$UProject = Join-Path $ProjectRoot "CombatForge.uproject"
$Exe = Join-Path $ProjectRoot "Binaries\Win64\CombatForge.exe"
if (-not (Test-Path $Exe)) { throw "Missing $Exe - build the game first." }

# Windowed standalone client. No -server. Boots to the menu; use ONLINE -> LOG IN -> SERVERS.
& $Exe $UProject "L_Graybox" "-game" "-windowed" "-ResX=1600" "-ResY=900"
