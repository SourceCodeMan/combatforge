// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Stable per-user data directories. Community maps used to live in <install>/Saved/Arenas, which every
 * re-package (or moving/deleting the packaged folder) silently wiped — maps now live in the OS per-user
 * settings dir (%LOCALAPPDATA%/CombatForge on Windows), shared by the editor and packaged builds, so the
 * map pool accumulates across sessions, repackages, and installs.
 */
struct PAINTFORGE_API FPFPaths
{
	/** .../CombatForge/Arenas — created on first call; one-time migration copies any legacy Saved/Arenas files in. */
	static const FString& ArenaDir();
};
