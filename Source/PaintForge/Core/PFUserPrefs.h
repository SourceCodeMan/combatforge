// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"   // FKey

/**
 * Local player prefs (GameUserSettings.ini [PaintForge]).
 * Loadout + extras for options that are not UGameUserSettings fields.
 */
struct PAINTFORGE_API FPFUserPrefs
{
	// ---- Loadout ----
	/** 0=Standard 12bps, 1=Rapid 14bps, 2=Tournament 10bps (always 30-mag / 150 total). */
	static int32 GetMarkerPreset();
	static void SetMarkerPreset(int32 Preset);

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

	// ---- Key rebinding (stored as "Bind_<ActionId>" = key name) ----
	/** Saved override key for a rebindable action, or an invalid FKey if none is saved. */
	static FKey GetKeyOverride(FName ActionId);
	static void SetKeyOverride(FName ActionId, FKey Key);
	/** Remove a saved override (revert that action to its default at next Build). */
	static void ClearKeyOverride(FName ActionId);

	static void Flush();

	static void ApplyMarkerPresetToWeapon(class UPFWeaponComponent* Weapon);
};
