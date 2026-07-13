// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

/**
 * A player's modular-character selection: one part index per customization slot (index into
 * PFChar::SlotParts(Slot); -1 = empty/none). Plain struct for now — becomes a USTRUCT for SaveGame +
 * replication in Phase 3/4.
 */
struct FPFCharacterConfig
{
	TArray<int32> Slots;
};

/**
 * Modular character-customization registry for the Bandit pack. Enumerates the skeletal-mesh parts under
 * each slot directory lazily (UObjectLibrary — loads asset DATA only, not the meshes), so the ~437 parts are
 * never hardcoded. The selected part per slot is loaded on demand.
 */
namespace PFChar
{
	int32 SlotCount();
	FName SlotId(int32 Slot);
	FString SlotLabel(int32 Slot);

	/** Enumerated part mesh paths for a slot (lazy-built + cached, sorted). */
	const TArray<FSoftObjectPath>& SlotParts(int32 Slot);

	/** Base body parts always mounted on top of SKM_Body (head + legs skin) to complete the naked body. */
	const TArray<FSoftObjectPath>& BaseParts();

	/** Load the selected part mesh for a slot (nullptr if index is out of range / -1 / load fails). */
	USkeletalMesh* LoadPart(int32 Slot, int32 Index);

	/** A sensible starting config (first option for the key visible slots, empty elsewhere). */
	FPFCharacterConfig DefaultConfig();

	/** For diagnostics: total parts enumerated across all slots. */
	int32 TotalPartCount();

	/** Readable name for a slot's part index ("None" for -1 / empty). */
	FString PartDisplayName(int32 Slot, int32 Index);

	// ---- Persistence (GGameUserSettings.ini [PaintForge]) ----
	void SaveConfig(const FPFCharacterConfig& Config);
	/** Load the saved config, or DefaultConfig() if none has been saved yet. */
	FPFCharacterConfig LoadConfig();
}
