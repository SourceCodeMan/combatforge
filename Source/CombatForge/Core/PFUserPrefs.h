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

	// Aim-down-sights input style: false = hold (default, press-and-hold), true = toggle (press to enter/exit).
	static bool GetADSToggle();
	static void SetADSToggle(bool bToggle);

	// Crouch input style: false = hold (default, press-and-hold), true = toggle (press to crouch/stand).
	static bool GetCrouchToggle();
	static void SetCrouchToggle(bool bToggle);

	static float GetFieldOfView();          // 80..110, default 105
	static void SetFieldOfView(float Fov);

	static float GetAmbientVolume();        // 0..1
	static void SetAmbientVolume(float V);

	// ---- User video grade (options sliders → the match lighting rig's post-process) ----
	static float GetBrightnessEV();         // -1..+1 EV offset on the rig's base exposure bias, default 0
	static void SetBrightnessEV(float EV);
	static float GetContrastScale();        // 0.85..1.20 scale on the rig's base contrast, default 1.0
	static void SetContrastScale(float C);

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
	static int32 GetResolutionIndex();      // 0..4 into the options table, default 4 (2560x1440) so borderless renders native on 1080p/1440p
	static void SetResolutionIndex(int32 Idx);
	static float GetResolutionScalePct();   // 50..100, default 100
	static void SetResolutionScalePct(float Pct);
	// Frame-rate cap. Uncapped renders as many frames as the GPU can draw — it pegged an RTX 5090 at ~90%
	// for zero gameplay gain (heat/noise/power). Index into {60, 120, 144, 240, Uncapped}, default 2 (144).
	static int32 GetFrameRateLimitIndex();
	static void SetFrameRateLimitIndex(int32 Idx);
	static float FrameRateLimitForIndex(int32 Idx);   // 0.f = uncapped (engine convention)

	/**
	 * Pipeline METHOD switches per quality level — Lumen vs SSGI vs none, Lumen reflections vs SSR, VSM vs
	 * CSM, TSR vs TAA/FXAA. These cvars are NOT ECVF_Scalability, so Scalability.ini cannot set them (the
	 * engine ensures + ignores); they must be pushed from code. Called at boot (GameInstance::Init) and on
	 * every Options quality Apply. Low(0)=cheapest, Medium(1)=screen-space, High(2)/Epic(3)=full pipeline.
	 */
	static void ApplyQualityMethodCVars(int32 QualityLevel);

	// ---- Gamepad / handheld ----
	// Right-stick look speed multiplier on the base 220°/s yaw / 150°/s pitch (0.2..3, default 1).
	static float GetGamepadLookScale();
	static void SetGamepadLookScale(float Scale);

	// Slate ApplicationScale multiplier (0.85..1.30, default 1) — handheld preset bumps it for 7-8" panels.
	static float GetUIScale();
	static void SetUIScale(float Scale);

	// Highest handheld-preset version ever auto-applied on this install (0 = never). The one-shot
	// guard that keeps the boot preset from stomping settings the player changed afterwards.
	static int32 GetHandheldPresetApplied();
	static void SetHandheldPresetApplied(int32 Version);

	// ---- Key rebinding (stored as "Bind_<ActionId>" = key name) ----
	/** Saved override key for a rebindable action, or an invalid FKey if none is saved. */
	static FKey GetKeyOverride(FName ActionId);
	static void SetKeyOverride(FName ActionId, FKey Key);
	/** Remove a saved override (revert that action to its default at next Build). */
	static void ClearKeyOverride(FName ActionId);

	// ---- Favorite community maps (host-side picker convenience, max 5) ----
	// Ids are the community catalog dedupe key: ArenaId (lowercase SHA1 content hash) when the
	// record has one, else FileName — the same rule ListTopCommunityMaps dedupes by.
	static constexpr int32 MaxFavoriteMaps = 5;
	static TArray<FString> GetFavoriteMapIds();
	static void SetFavoriteMapIds(const TArray<FString>& Ids);

	static void Flush();

};
