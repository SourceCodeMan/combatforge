// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/PaintForgeTypes.h"

class FJsonObject;

/**
 * Arena fingerprints (T27) + the ArenaLayout JSON piece/grid block (B13, schema v1).
 * The rating subsystem (pkg-meta) owns file I/O and appends the result/votes blocks; this is
 * the pure data side. Field names are EXACT per contract §3.7 — append-only evolution.
 */
struct PAINTFORGE_API FPFArenaSerialization
{
	/**
	 * arenaId: lowercase SHA1-hex over the grid header + pieces sorted by
	 * (Type, X, Y, Z, Rot, Team), EXCLUDING PieceId/Owner — identical rebuilds hash identically
	 * regardless of who placed what (T27).
	 */
	static FString ComputeArenaId(const TArray<FPFBuildPieceRec>& Pieces);

	/**
	 * halfHashA/B: same algorithm over the team-filtered subset, additionally excluding Team.
	 */
	static FString ComputeHalfHash(const TArray<FPFBuildPieceRec>& Pieces, uint8 Team);

	/**
	 * Builds the ① piece/grid block of the match JSON: schema, game, matchId, createdUtc,
	 * teamSize, grid{...}, arenaId, halfHashA/B, pieces[{id,t,x,y,z,r,own,team}].
	 */
	static TSharedRef<FJsonObject> BuildLayoutJson(const TArray<FPFBuildPieceRec>& Pieces,
	                                               const FString& MatchId, int32 TeamSize,
	                                               const FDateTime& CreatedUtc);
};
