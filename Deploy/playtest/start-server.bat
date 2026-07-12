@echo off
REM One-click headless-style server (join with a separate client on this or another PC).
cd /d "%~dp0\..\.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-server.ps1" %*
