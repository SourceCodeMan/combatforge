// Copyright (c) 2026 Tom Chapman. All rights reserved.

using UnrealBuildTool;

public class CombatForge : ModuleRules
{
	public CombatForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bWarningsAsErrors = true;   // contract §5.17: warnings-as-errors stays on

		// Flat module layout (Source/CombatForge/<Subsystem>/…) with module-root-relative
		// includes like "Core/CombatForgeTypes.h" — put the module root on the include path
		// so those resolve (UE 5.6 doesn't add it automatically without Public/Private folders).
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore",
			"EnhancedInput",
			"UMG", "Slate", "SlateCore",
			"NetCore", "Sockets",                       // push-model / net serialization helpers
			"EngineCameras",                 // UWaveOscillatorCameraShakePattern (PFCameraShakes)
			"AIModule",                      // AAIController base for roster-filling bots (PFBotController)
			"GameplayTasks",                 // AIModule dep for AI Perception (sight/hearing senses)
			"NavigationSystem",              // runtime navmesh + nav invokers + MoveTo pathfinding (dynamic built arena)
			"Json", "JsonUtilities",         // arena rating records
			"RenderCore",                    // GShaderCompilingManager / loading-screen shader drain
			"RHI",                           // shader pipeline cache helpers
			"AssetRegistry",                 // UObjectLibrary part enumeration (PFCharacterCustomization)
			"Niagara",                       // muzzle / impact VFX (soft-ref systems + mesh fallback)
			"HTTP",                          // combatforge-api client (accounts / server browser / reports)
			"OpenSSL"                        // HMAC-SHA256 match-report signing (fleet mode only)
		});
		// Editor builds only: GEditor is consulted by the FP body-visibility sync to detect F8
		// eject/simulate — the ejected editor camera moves without touching the game's own camera
		// manager or view target, so the game has no other way to notice it is being LOOKED AT.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		// No private-only deps. NOT needed: OnlineSubsystem (02 D12) — the backend is plain REST
		// against api.playcombatforge.com (docs/multiplayer-plan.md), GameplayAbilities (overkill).
		// ALSO not needed: MediaAssets + AudioMixer. Both were added for the Media Framework phase-BGM
		// path (UMediaSoundComponent over the loose .mp3s), which was DROPPED — PFMusicSubsystem plays
		// cooked SoundWave assets through the normal audio engine and touches neither module. The
		// MediaPlate + WmfMedia plugins went with them (WmfMedia is Windows-only, so it was also dead
		// weight on the Mac build). Removed 2026-07-26 before the store beta.
	}
}
