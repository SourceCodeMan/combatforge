// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/CombatForgeTypes.h"

class FJsonObject;

/**
 * Arena fingerprints (T27) + the ArenaLayout JSON piece/grid block (B13, schema v1).
 * The rating subsystem (pkg-meta) owns file I/O and appends the result/votes blocks; this is
 * the pure data side. Field names are EXACT per contract §3.7 — append-only evolution.
 */
struct COMBATFORGE_API FPFArenaSerialization
{
	/**
	 * arenaId: lowercase SHA1-hex over the grid header + pieces sorted by
	 * (Type, X, Y, Z, Rot, Team), EXCLUDING PieceId/Owner — identical rebuilds hash identically
	 * regardless of who placed what (T27).
	 *
	 * P2-BD1: the grid header hashes the ACTIVE map's rows (+ its derived level count, same rule
	 * as BuildLayoutJson) — Warehouse and Yard layouts with identical piece lists used to collide
	 * on one id. Warehouse ids are unchanged (CellsY 10 is the default header); Yard ids fork.
	 */
	static FString ComputeArenaId(const TArray<FPFBuildPieceRec>& Pieces,
	                              int32 GridCellsY = PFGrid::CellsY);

	/**
	 * halfHashA/B: same algorithm over the team-filtered subset, additionally excluding Team.
	 */
	static FString ComputeHalfHash(const TArray<FPFBuildPieceRec>& Pieces, uint8 Team,
	                               int32 GridCellsY = PFGrid::CellsY);

	/**
	 * Builds the ① piece/grid block of the match JSON: schema, game, matchId, createdUtc,
	 * teamSize, grid{...}, arenaId, halfHashA/B, pieces[{id,t,x,y,z,r,own,team}].
	 *
	 * ParentArenaId (Remix lineage, T27+): the arenaId of the community map this layout was built ON TOP
	 * of (Improvement/Remix mode). Written as "parentArenaId" ONLY when it is non-empty AND differs from
	 * this layout's own arenaId — so a remix that changed nothing (or a from-scratch/Creative build) records
	 * no parent. This makes "improve → NEW map, never overwrite the original" explicit and traceable in the
	 * data (the content-hash arenaId already guarantees a changed layout is a distinct map + distinct file).
	 */
	static TSharedRef<FJsonObject> BuildLayoutJson(const TArray<FPFBuildPieceRec>& Pieces,
	                                               const FString& MatchId, int32 TeamSize,
	                                               const FDateTime& CreatedUtc,
	                                               int32 GridCellsY,
	                                               const FString& ParentArenaId = FString());

	/**
	 * Inverse of BuildLayoutJson's piece block: parses the "pieces" array back into records (and reads
	 * "teamSize"). PieceId is read but the caller MUST re-mint ids before injecting into a live grid.
	 * Returns false if there are no valid pieces.
	 */
	static bool ParseLayoutJson(const TSharedRef<FJsonObject>& Root,
	                            TArray<FPFBuildPieceRec>& OutPieces, int32& OutTeamSize);
};
