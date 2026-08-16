// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"

/** What kind of handheld (if any) this client is running on. */
enum class EPFHandheldKind : uint8
{
	None,              // desktop / laptop — leave everything alone
	WindowsHandheld,   // ROG Ally / Legion Go / other Ryzen Z-series Windows handhelds
	SteamDeck,         // Steam Deck (SteamOS+Proton, or Windows installed on the Deck)
};

/**
 * Handheld detection + one-time performance preset (client-only, zero replication).
 *
 * Detection is deliberately CONSERVATIVE: only explicit signals (launch args, the
 * SteamDeck env var, or a known handheld APU brand string) count. A battery + small
 * screen heuristic was rejected — it would misfire on ordinary laptops and silently
 * downgrade their settings.
 *
 * The preset applies ONCE per preset version (guarded by a saved pref), so a player
 * who raises their settings afterwards is never stomped on the next boot. Launch args:
 *   -pfhandheld      force Windows-handheld detection
 *   -pfsteamdeck     force Steam Deck detection
 *   -pfnothandheld   force OFF (wins over everything)
 * Console: `pf.HandheldInfo` logs the signals; `pf.HandheldPreset` re-applies the preset.
 */
struct COMBATFORGE_API FPFHandheldPlatform
{
	/** Cached after the first call. Safe from any thread after that (game thread first). */
	static EPFHandheldKind Detect();

	static bool IsHandheld() { return Detect() != EPFHandheldKind::None; }

	static const TCHAR* KindName(EPFHandheldKind Kind);

	/**
	 * Boot hook (GameInstance::Init, before the frame-cap block so the cap pref it
	 * writes is picked up by the existing apply): detect + log, apply the first-run
	 * preset if this is a handheld, and apply the saved UI scale.
	 */
	static void InitAtBoot();

	/** Write the handheld preset prefs AND push them into the live engine settings. */
	static void ApplyHandheldPreset(EPFHandheldKind Kind);

	/** Push the saved UI scale pref into Slate (every boot + options Apply). */
	static void ApplyUIScaleFromPrefs();
};
