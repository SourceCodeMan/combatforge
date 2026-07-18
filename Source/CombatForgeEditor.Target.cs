// Copyright (c) 2026 Tom Chapman. All rights reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class CombatForgeEditorTarget : TargetRules
{
	public CombatForgeEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
		ExtraModuleNames.Add("CombatForge");

		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			// macOS only: Xcode 26.6 is newer than UE 5.6's Apple_SDK.json MaxVersion (16.9.0), so the
			// project declares Config/Mac/Mac_SDK.json { MainVersion: 26.6 } to pass the SDK version gate.
			// The editor is a MODULAR target on a Shared build environment, and UBT rejects a per-project
			// SDK override there (AllowsPerProjectSDKVersion() == false) -> "this target is not allowed"
			// RulesError, which is what blocks opening CombatForge.uproject on a Mac.
			//
			// This flag is the sanctioned opt-in: it lets SDK-version-sensitive modules get a project-side
			// copy under a Shared environment. Safe here because we aren't switching toolchains — clang is
			// the same either way; we're only bypassing a version-RANGE check. (Note: the engine's own error
			// text suggests bAreTargetSDKVersionsRelevantOverride = false, but that does NOT work for this
			// case — IsSDKVersionRelevant() returns true unconditionally when the target's platform IS the
			// SDK's platform, so the flag is never consulted.)
			bAllowSDKOverrideModulesWithSharedEnvironment = true;
		}
	}
}
