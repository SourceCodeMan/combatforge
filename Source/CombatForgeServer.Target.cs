// Copyright (c) 2026 Tom Chapman. All rights reserved.

using UnrealBuildTool;
using System.Collections.Generic;

/// <summary>
/// Headless dedicated server target (playtest / Docker / VPS).
///
/// NOTE: Epic Launcher binary distributions of UE 5.6 reject this target:
///   "Server targets are not currently supported from this engine distribution."
/// Use Deploy/playtest/run-listen.ps1 or run-server.ps1 (game exe + -server -nullrhi)
/// until the project is built against a source-built engine.
///
/// Build (source engine only):
///   Build.bat CombatForgeServer Win64 Development -project=...
/// Same CombatForge module as the game client — no gameplay code changes.
/// </summary>
public class CombatForgeServerTarget : TargetRules
{
	public CombatForgeServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
		ExtraModuleNames.Add("CombatForge");
	}
}
