// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Stable per-user data directories. Community maps used to live in <install>/Saved/Arenas, which every
 * re-package (or moving/deleting the packaged folder) silently wiped — maps now live in the OS per-user
 * settings dir (%LOCALAPPDATA%/CombatForge on Windows), shared by the editor and packaged builds, so the
 * map pool accumulates across sessions, repackages, and installs.
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
};
