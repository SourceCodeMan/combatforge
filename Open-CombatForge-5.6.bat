@echo off
REM Opens CombatForge directly in UE 5.6 — bypasses the Epic Launcher's Project
REM Browser (which wrongly offers to "convert" because it mis-resolves the 5.6
REM engine association). This always opens clean, no upgrade prompt.
start "" "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe" "D:\projects\combatforge\CombatForge.uproject"
