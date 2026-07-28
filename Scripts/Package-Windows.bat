@echo off
REM ============================================================================
REM  Combat Forge - package a standalone WINDOWS build.
REM  Output: Packaged\Windows\  (a self-contained folder: CombatForge.exe + content)
REM
REM  Run with the EDITOR CLOSED (it locks the game DLL). Takes a while on a cold
REM  cook (shaders + the 12 GB Bandit/warehouse content). See docs\packaging.md.
REM
REM  Build config: Development (console + pf.* debug cvars). For a clean release
REM  build pass "Shipping" as the first argument:  Package-Windows.bat Shipping
REM ============================================================================
setlocal enabledelayedexpansion

REM UE root: env var wins so a non-default install doesn't require editing this file (issue #20 S5).
if not defined UE set "UE=C:\Program Files\Epic Games\UE_5.6"
set "PROJ=%~dp0..\CombatForge.uproject"
set "OUT=%~dp0..\Packaged\Windows"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Development"

REM Shipping builds are the ones that go to players - mark them for distribution (issue #21 D4).
set "DISTFLAG="
if /I "%CONFIG%"=="Shipping" set "DISTFLAG=-distribution"

echo.
echo === Packaging Combat Forge (Windows / %CONFIG%) ===
echo     Project: %PROJ%
echo     Output:  %OUT%
echo.

REM --- NetProtocol reminder (issue #19 I7) -----------------------------------------
REM Every itch push MUST bump PFBuild::NetProtocol so stale clients are rejected at the
REM join handshake instead of silently mis-rendering. Surface the current value here so
REM the packager can't miss it.
findstr /C:"NetProtocol" "%~dp0..\Source\CombatForge\CombatForge.h"
echo *** REMINDER: bump PFBuild::NetProtocol (above) if this package goes to itch. ***
echo.

REM --- Optional HARD GATE: set PF_REQUIRE_PROTOCOL_BUMP=1 to refuse packaging at a protocol
REM that was already packaged. The reminder above is advisory and easy to scroll past; this
REM compares against the value recorded by the last successful package and stops. (P2-S4)
set "PROTO="
for /f "tokens=2 delims==;" %%P in ('findstr /R /C:"NetProtocol *= *[0-9]" "%~dp0..\Source\CombatForge\CombatForge.h"') do (
  if not defined PROTO for /f "tokens=1" %%Q in ("%%P") do set "PROTO=%%Q"
)
set "PROTOSTAMP=%~dp0..\Packaged\.last-packaged-protocol"
set "LASTPROTO="
if exist "%PROTOSTAMP%" set /p LASTPROTO=<"%PROTOSTAMP%"
if "%PF_REQUIRE_PROTOCOL_BUMP%"=="1" if defined PROTO if defined LASTPROTO (
  if "!PROTO!"=="!LASTPROTO!" (
    echo.
    echo *** REFUSING TO PACKAGE: NetProtocol is still !PROTO!, the value already packaged.
    echo *** Bump PFBuild::NetProtocol in Source\CombatForge\CombatForge.h, or unset
    echo *** PF_REQUIRE_PROTOCOL_BUMP to package anyway.
    exit /b 2
  )
)

call "%UE%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun ^
  -project="%PROJ%" ^
  -noP4 -utf8output -nocompileeditor ^
  -platform=Win64 ^
  -clientconfig=%CONFIG% %DISTFLAG% ^
  -build -cook -stage -pak -iostore -compressed ^
  -nodebuginfo ^
  -prereqs ^
  -archive -archivedirectory="%OUT%"

echo.
if %ERRORLEVEL% NEQ 0 (
  echo *** PACKAGE FAILED (exit %ERRORLEVEL%) - see the UAT log above. ***
  endlocal
  exit /b 1
)

REM Success only: record the protocol this package shipped at, so PF_REQUIRE_PROTOCOL_BUMP can
REM detect a re-package at the same value next time. Must stay BELOW the failure check - mkdir
REM and echo reset ERRORLEVEL, so writing the stamp above it would mask a failed package. (P2-S4)
if defined PROTO (
  if not exist "%~dp0..\Packaged" mkdir "%~dp0..\Packaged"
  >"%PROTOSTAMP%" echo %PROTO%
)

REM --- Privacy scrub (Adam/Benj alpha, 2026-07-15) --------------------------------
REM Saved\ regenerates every time the packaged exe runs (playtest logs/crash dumps)
REM and leaks the host's PC name + LAN IP + hardware; .pdb leaks build paths. Strip
REM both from the archive so a butler push can never ship them. This is the durable
REM fix behind the manual delete that was being done by hand before each push.
if exist "%OUT%\CombatForge\Saved" rmdir /s /q "%OUT%\CombatForge\Saved"
if exist "%OUT%\Engine\Saved"      rmdir /s /q "%OUT%\Engine\Saved"
del /s /q "%OUT%\*.pdb" >nul 2>&1

REM --- Pre-push guard: refuse to leave anything sensitive in the archive ----------
set "DIRTY="
if exist "%OUT%\CombatForge\Saved" set "DIRTY=1"
dir /s /b "%OUT%\*.pdb" >nul 2>&1 && set "DIRTY=1"
if defined DIRTY (
  echo *** WARNING: Saved or pdb still present in %OUT% - do NOT push until clean. ***
) else (
  echo === Done + scrubbed. Build is in %OUT% - no Saved, no pdb. ===
)
endlocal
