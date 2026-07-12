// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Writes a few playable community arenas under Saved/Arenas/ when missing.
 * Ensures first-time installs (and packaged playtests) have maps for
 * Improvement / Play-Only without relying on prior host builds.
 */
struct PAINTFORGE_API FPFArenaSeed
{
	/** Idempotent: creates seed_*.json only if those files are absent. */
	static int32 EnsureSeedArenas();
};
