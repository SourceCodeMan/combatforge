// Copyright (c) 2026 Tom Chapman. All rights reserved.

using UnrealBuildTool;

public class PaintForge : ModuleRules
{
	public PaintForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bWarningsAsErrors = true;   // contract §5.17: warnings-as-errors stays on

		// Flat module layout (Source/PaintForge/<Subsystem>/…) with module-root-relative
		// includes like "Core/PaintForgeTypes.h" — put the module root on the include path
		// so those resolve (UE 5.6 doesn't add it automatically without Public/Private folders).
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore",
			"EnhancedInput",
			"UMG", "Slate", "SlateCore",
			"NetCore",                       // push-model / net serialization helpers
			"EngineCameras",                 // UWaveOscillatorCameraShakePattern (PFCameraShakes)
			"AIModule",                      // AAIController base for roster-filling bots (PFBotController)
			"GameplayTasks",                 // AIModule dep for AI Perception (sight/hearing senses)
			"NavigationSystem",              // runtime navmesh + nav invokers + MoveTo pathfinding (dynamic built arena)
			"Json", "JsonUtilities",         // arena rating records
			"RenderCore",                    // GShaderCompilingManager / loading-screen shader drain
			"RHI",                           // shader pipeline cache helpers
			"AssetRegistry"                  // UObjectLibrary part enumeration (PFCharacterCustomization)
		});
		// No private-only deps in v1. NOT needed: OnlineSubsystem (02 D12),
		// Niagara (post-v1 art), GameplayAbilities (overkill).
	}
}
