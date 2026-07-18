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

	/**
	 * Visible base SKIN parts, mounted as followers of the leader mesh.
	 *
	 * ⚠️ SKM_Body is a ONE-PIECE naked body — it already contains torso + arms + LEGS (its material is
	 * M_Body_FULL and PA_Body_PhysicsAsset carries thigh/calf/foot bodies, unlike the torso-only PA_Torso).
	 * It is therefore used ONLY as the skeleton/anim/bounds carrier and is NOT rendered. The visible skin is
	 * this modular set, so a REGION can be hidden when a garment covers it — that is what makes it possible to
	 * drop the bare legs under jeans instead of having skin poke through the inner thigh (Tom 2026-07-18).
	 * Do NOT "fix" leg clipping with HideBoneByName: followers inherit the LEADER's bone visibility, so hiding
	 * thigh bones would collapse the trousers along with the skin.
	 * Order MUST match the kBase* indices below.
	 */
	const TArray<FSoftObjectPath>& BaseParts();

	// Base skin part indices (order of BaseParts()).
	constexpr int32 kBaseHead = 0, kBaseTorso = 1, kBaseArms = 2, kBaseLegs = 3;
	constexpr int32 kBasePartCount = 4;
	// GSlots indices used by garment-vs-skin hiding.
	constexpr int32 kSlotPants = 6;

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
