// Copyright (c) 2026 Tom Chapman. All rights reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class PaintForgeTarget : TargetRules
{
	public PaintForgeTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
		ExtraModuleNames.Add("PaintForge");
	}
}
