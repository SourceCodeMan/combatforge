// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/CombatForgeTypes.h"

namespace PFVoteCategories
{
	// Fixed IDs 1..8 (T13). Index in All = ID - 1. Stored on disk as these lowercase strings —
	// never localize, never renumber (01 §4.1).
	const TArray<FName> All =
	{
		FName(TEXT("layout")),
		FName(TEXT("cover")),
		FName(TEXT("verticality")),
		FName(TEXT("flow")),
		FName(TEXT("balance")),
		FName(TEXT("sightlines")),
		FName(TEXT("creativity")),
		FName(TEXT("pacing"))
	};

	// Player-facing hints per 01 §4.1. Culture-invariant on purpose: display-only graybox text;
	// localization is a post-v1 pass and the stored IDs above never localize regardless.
	const TArray<FText> HintText =
	{
		INVTEXT("Overall shape and lanes"),
		INVTEXT("Amount and placement of cover"),
		INVTEXT("Ramps, towers, high ground"),
		INVTEXT("How movement and routes felt"),
		INVTEXT("Fair for both sides"),
		INVTEXT("Long shots vs close quarters mix"),
		INVTEXT("Original, clever, fun to look at"),
		INVTEXT("Rounds felt fast / right length")
	};

	FName FromId(uint8 Id)
	{
		if (Id >= 1 && Id <= 8)
		{
			return All[Id - 1];
		}
		return NAME_None;
	}
}

namespace PFColors
{
	FLinearColor ForTeam(uint8 Team)
	{
		return (Team == 0) ? TeamA : TeamB;
	}
}

const FPFArenaMapDef& PFGetArenaMapDef(EPFArenaMap Map)
{
	// Compile-time truths per map (task #40). Warehouse values MUST reproduce the pre-selector
	// arena exactly (field 6400×4000, sun 6.0 — the Tom-validated frozen rig); the Yard doubles the
	// WIDTH only (same 6400 length, so plots/spawn columns/X-mirror math are untouched), drops the
	// roof, and runs the sun at 4.0 because a roofless field is 100% sun pool — 6.0 open-air would
	// blow the floor out with exposure locked (see the 8.5 regression note in PFLightingSubsystem).
	static const FPFArenaMapDef Warehouse = {
		EPFArenaMap::Warehouse,
		static_cast<float>(PFGrid::CellsX * PFGrid::CellUU),        // 6400
		static_cast<float>(PFGrid::CellsY * PFGrid::CellUU),        // 4000
		/*bRoof=*/true, /*bWarehouseScenery=*/false,
		/*SunIntensity=*/6.f, TEXT("WAREHOUSE")
	};
	static const FPFArenaMapDef Yard = {
		EPFArenaMap::Yard,
		static_cast<float>(PFGrid::CellsX * PFGrid::CellUU),        // 6400 — length unchanged
		static_cast<float>(2 * PFGrid::CellsY * PFGrid::CellUU),    // 8000 — double width
		/*bRoof=*/false, /*bWarehouseScenery=*/true,
		/*SunIntensity=*/4.f, TEXT("THE YARD")
	};
	return (Map == EPFArenaMap::Yard) ? Yard : Warehouse;
}
