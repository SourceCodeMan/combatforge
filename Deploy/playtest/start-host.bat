@echo off
REM One-click LISTEN host for LAN/VPN playtest (you play + others join).
cd /d "%~dp0\..\.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-listen.ps1" %*
