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
set "PROJ=D:\projects\paintforge\PaintForge.uproject"
set "OUT=D:\projects\paintforge\Packaged\Windows"

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
  -prereqs ^
  -archive -archivedirectory="%OUT%"

echo.
if %ERRORLEVEL% NEQ 0 (
  echo *** PACKAGE FAILED (exit %ERRORLEVEL%) - see the UAT log above. ***
) else (
  echo === Done. Build is in %OUT% ===
)
endlocal
