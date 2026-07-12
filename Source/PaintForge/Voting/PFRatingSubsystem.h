// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Dom/JsonObject.h"
#include "Misc/DateTime.h"
#include "Core/PaintForgeTypes.h"
#include "PFRatingSubsystem.generated.h"

/**
 * Match record lifecycle + vote persistence (contract §3.7, B13, T24).
 *
 * SERVER-ONLY lifecycle (host process) — every public call is a no-op on pure clients.
 * One JSON file per match under Saved/Arenas/:
 *   - BeginMatchRecord (Build→Combat): writes the initial file with the frozen arena layout,
 *     fingerprints, and grid header (via FPFArenaSerialization).
 *   - AddVote (VotePhase): collects one FPFVoteRecord per player (GameMode dedupes upstream).
 *   - CommitMatchRecord (Vote→Results): appends the "result" and "votes" blocks (rewriting the
 *     same file) and clears state.
 */
UCLASS()
class PAINTFORGE_API UPFRatingSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * Build→Combat: computes fingerprints + the layout JSON via FPFArenaSerialization and writes
	 * the initial file Saved/Arenas/arena_<UTCyyyyMMdd_HHmmss>_<matchId first 8 hex>.json.
	 * Any record still open from a previous match is discarded with a warning.
	 */
	void BeginMatchRecord(const FString& MatchId, const TArray<FPFBuildPieceRec>& FrozenPieces,
	                      int32 TeamSize);

	/**
	 * VotePhase: stages one player's vote for the commit. bBuiltHalfA = the voter's team built
	 * half A ("voterBuiltHalf" in the JSON). One per player — GameMode dedupes; a repeated
	 * VoterGuidHash overwrites the staged entry defensively.
	 */
	void AddVote(const FPFVoteRecord& Vote, bool bBuiltHalfA);

	/**
	 * Vote→Results: appends the result + votes blocks to the layout JSON, rewrites the match
	 * file, and clears all record state.
	 */
	void CommitMatchRecord(const FPFMatchResult& Result);

	/** SHA1-hex arena fingerprint of the active record; empty outside an active record. */
	FString GetCurrentArenaId() const;

	/**
	 * Picks a saved community arena from Saved/Arenas/, takes its more-developed half, and remaps that
	 * half into TargetTeam's plot (X translation between plots) so it can be injected for an all-bot
	 * team. Server-only; returns false if no saved arena exists. (v1 picks the most-recent file; vote
	 * ranking can refine the pick later.)
	 */
	bool PickCommunityHalf(TArray<FPFBuildPieceRec>& OutHalf, uint8 TargetTeam) const;

private:
	/** One staged vote (AddVote input, held until CommitMatchRecord). Not a USTRUCT: no GC refs. */
	struct FPFPendingVote
	{
		FPFVoteRecord Vote;
		bool bBuiltHalfA = false;
	};

	/** True on listen server / standalone host; false on pure clients (no world = false). */
	bool IsServerContext() const;

	/** Serializes CurrentRecordJson to CurrentFilePath. Logs and returns false on failure. */
	bool WriteRecordToDisk() const;

	/** Drops all in-progress record state. */
	void ClearRecordState();

	/** Layout JSON from FPFArenaSerialization; result/votes blocks are appended at commit. */
	TSharedPtr<FJsonObject> CurrentRecordJson;

	/** Staged votes, committed (with voterWonMatch resolved) in CommitMatchRecord. */
	TArray<FPFPendingVote> PendingVotes;

	FString CurrentMatchId;
	FString CurrentArenaId;
	FString CurrentFilePath;
	FDateTime RecordCreatedUtc;
	bool bRecordActive = false;
};
