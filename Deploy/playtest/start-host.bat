@echo off
REM One-click LISTEN host for LAN/VPN playtest (you play + others join).
REM PreferEditor: most reliable input path for local playtests (avoids stale Binaries game exe).
cd /d "%~dp0\..\.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-listen.ps1" -PreferEditor %*
