// Copyright (c) 2026 Tom Chapman. All rights reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class PaintForgeEditorTarget : TargetRules
{
	public PaintForgeEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
		ExtraModuleNames.Add("PaintForge");
	}
}
