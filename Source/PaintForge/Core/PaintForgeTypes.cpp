// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PaintForgeTypes.h"

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
