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
setlocal

set "UE=C:\Program Files\Epic Games\UE_5.6"
set "PROJ=%~dp0..\CombatForge.uproject"
set "OUT=%~dp0..\Packaged\Windows"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Development"

echo.
echo === Packaging Combat Forge (Windows / %CONFIG%) ===
echo     Project: %PROJ%
echo     Output:  %OUT%
echo.

call "%UE%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun ^
  -project="%PROJ%" ^
  -noP4 -utf8output -nocompileeditor ^
  -platform=Win64 ^
  -clientconfig=%CONFIG% ^
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
  echo *** WARNING: Saved\ or .pdb still present in %OUT% - do NOT push until clean. ***
) else (
  echo === Done + scrubbed. Build is in %OUT% (no Saved/, no .pdb - safe to push). ===
)
endlocal
