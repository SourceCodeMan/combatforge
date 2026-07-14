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

	// ---- Persistence (GGameUserSettings.ini [CombatForge]) ----
	/** Save/load the ACTIVE save slot (convenience — routes to GetActiveSaveSlot()). */
	void SaveConfig(const FPFCharacterConfig& Config);
	/** Load the active slot's config, or DefaultConfig() if that slot has never been saved. */
	FPFCharacterConfig LoadConfig();

	// ---- CoD-style save slots: pick one, edit it, spawn with it ----
	/** Number of character save slots. */
	int32 SaveSlotCount();
	void SaveConfig(int32 SaveSlot, const FPFCharacterConfig& Config);
	FPFCharacterConfig LoadConfig(int32 SaveSlot);
	/** The save slot the pawn spawns with (persisted, clamped to [0, SaveSlotCount()-1]). */
	int32 GetActiveSaveSlot();
	void SetActiveSaveSlot(int32 SaveSlot);
}
