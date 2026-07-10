// Copyright (c) 2026 Tom Chapman. All rights reserved.

using UnrealBuildTool;

public class PaintForge : ModuleRules
{
	public PaintForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bWarningsAsErrors = true;   // contract §5.17: warnings-as-errors stays on

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore",
			"EnhancedInput",
			"UMG", "Slate", "SlateCore",
			"NetCore",                       // push-model / net serialization helpers
			"Json", "JsonUtilities"          // arena rating records
		});
		// No private-only deps in v1. NOT needed: OnlineSubsystem (02 D12),
		// Niagara (post-v1 art), GameplayAbilities (overkill), AIModule.
	}
}
