// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Stable data directories for community maps + server-side state.
 *
 * Desktop (listen host / solo): %LOCALAPPDATA%/CombatForge/Arenas (UserSettingsDir).
 * Dedicated / -NOHOMEDIR / -nullrhi: %ProgramData%/CombatForge/Arenas (or -ArenaDir= override) —
 * NEVER the in-install Saved/ tree, which every redeploy wipes.
 */
struct COMBATFORGE_API FPFPaths
{
	/** .../CombatForge/Arenas — created on first call; one-time migration copies any legacy Saved/Arenas files in. */
	static const FString& ArenaDir();

	/**
	 * Persistent PARENT of ArenaDir (e.g. .../CombatForge, or the -ArenaDir override's parent). Server-side
	 * state that must survive a redeploy — the fleet ServerKey.txt, un-sent XP match reports (PendingReports),
	 * the private-match JoinCode — lives here instead of ProjectSavedDir (which is inside the packaged build and
	 * is wiped on every server update, taking un-minted XP with it). Honors -ArenaDir on the dedicated box. (#8)
	 */
	static FString ServerDataDir();

	/**
	 * .../CombatForge/UserPrefs.ini — the player's OWN settings: classes, weapons, key bindings,
	 * audio/video prefs. Everything CombatForge writes under the [CombatForge] section lives here.
	 *
	 * These used to live in the packaged build's Saved/Config/GameUserSettings.ini, which is INSIDE
	 * the install and is deleted by the pre-ship scrub on every bake — so a fresh build always came
	 * up with default classes until the player set them again (Tom 2026-07-20). Same class of bug as
	 * the community maps in ArenaDir, and the same fix: keep player data beside the maps, outside
	 * anything a redeploy or a scrub can touch. Also stops a packaged build shipping the packager's
	 * personal loadout to every player.
	 *
	 * Engine settings (resolution, scalability) deliberately STAY in GameUserSettings.ini — those are
	 * per-install and should reset with the hardware they were tuned for.
	 *
	 * On first call, any existing [CombatForge] keys in GameUserSettings.ini are migrated across once.
	 */
	static const FString& UserPrefsIni();
};
