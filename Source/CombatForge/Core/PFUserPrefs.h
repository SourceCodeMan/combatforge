// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"   // FKey

/**
 * Local player prefs (GameUserSettings.ini [CombatForge]).
 * Loadout + extras for options that are not UGameUserSettings fields.
 */
struct COMBATFORGE_API FPFUserPrefs
{
	// ---- Loadout ----
	/** 0=Standard 12bps, 1=Rapid 14bps, 2=Tournament 10bps (always 30-mag / 150 total). */

	/** 0=Cross+dot, 1=Dot only, 2=Cross only */
	static int32 GetCrosshairStyle();
	static void SetCrosshairStyle(int32 Style);

	// ---- Options extras ----
	static bool GetInvertY();
	static void SetInvertY(bool bInvert);

	static float GetFieldOfView();          // 80..110, default 105
	static void SetFieldOfView(float Fov);

	static float GetAmbientVolume();        // 0..1
	static void SetAmbientVolume(float V);

	static int32 GetWindowModeIndex();      // 0=Fullscreen 1=Borderless 2=Windowed
	static void SetWindowModeIndex(int32 Idx);

	// LAN multiplayer: the last host IP joined (pre-filled in the JOIN box).
	static FString GetLastJoinIp();
	static void SetLastJoinIp(const FString& Ip);

	// Video prefs mirrored here because the ENGINE readbacks are lossy: GetOverallScalabilityLevel() returns
	// -1 whenever a custom resolution scale is applied (the widget clamped that to Low and then SAVED Low on
	// the next Apply). The user's true choices seed the UI; the engine values are derived from them.
	static int32 GetQualityLevel();         // 0..3, default 2 (High)
	static void SetQualityLevel(int32 Level);
	static int32 GetResolutionIndex();      // 0..4 into the options table, default 3 (1920x1080)
	static void SetResolutionIndex(int32 Idx);
	static float GetResolutionScalePct();   // 50..100, default 100
	static void SetResolutionScalePct(float Pct);
	// Frame-rate cap. Uncapped renders as many frames as the GPU can draw — it pegged an RTX 5090 at ~90%
	// for zero gameplay gain (heat/noise/power). Index into {60, 120, 144, 240, Uncapped}, default 2 (144).
	static int32 GetFrameRateLimitIndex();
	static void SetFrameRateLimitIndex(int32 Idx);
	static float FrameRateLimitForIndex(int32 Idx);   // 0.f = uncapped (engine convention)

	// ---- Key rebinding (stored as "Bind_<ActionId>" = key name) ----
	/** Saved override key for a rebindable action, or an invalid FKey if none is saved. */
	static FKey GetKeyOverride(FName ActionId);
	static void SetKeyOverride(FName ActionId, FKey Key);
	/** Remove a saved override (revert that action to its default at next Build). */
	static void ClearKeyOverride(FName ActionId);

	static void Flush();

};
