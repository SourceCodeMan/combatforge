@echo off
REM Compatibility wrapper. The single Windows packaging implementation lives in
REM Deploy\playtest\package-playtest.ps1 so playtest, itch, and Epic builds cannot drift.
setlocal

if not defined UE set "UE=C:\Program Files\Epic Games\UE_5.6"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Development"

if /I not "%CONFIG%"=="Development" if /I not "%CONFIG%"=="Shipping" (
  echo Usage: Package-Windows.bat [Development^|Shipping]
  exit /b 2
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\Deploy\playtest\package-playtest.ps1" ^
  -Config "%CONFIG%" -Engine "%UE%"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" echo *** PACKAGE FAILED ^(exit %RC%^) ***
endlocal & exit /b %RC%
