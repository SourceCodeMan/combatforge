// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Dom/JsonObject.h"
#include "Misc/DateTime.h"
#include "Core/CombatForgeTypes.h"
#include "PFRatingSubsystem.generated.h"

/** One ranked community arena for the pregame map picker (host disk catalog). */
USTRUCT()
struct COMBATFORGE_API FPFCommunityMapInfo
{
	GENERATED_BODY()

	/** Filename only under Saved/Arenas/ (e.g. arena_20260712_….json). */
	UPROPERTY() FString FileName;
	UPROPERTY() FString ArenaId;
	/** Short human label for UI lists. */
	UPROPERTY() FString DisplayName;
	/** Vote score (up=+2, down=−1) + piece-count soft boost. */
	UPROPERTY() int32 Score = 0;
	UPROPERTY() int32 PieceCount = 0;
	UPROPERTY() int32 TeamSize = 0;
	UPROPERTY() int32 ThumbUp = 0;
	UPROPERTY() int32 ThumbDown = 0;
	UPROPERTY() FString CreatedUtc;
};

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
class COMBATFORGE_API UPFRatingSubsystem : public UGameInstanceSubsystem
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

	/**
	 * Loads a whole saved community arena (both halves, unchanged) for Improvement mode — everyone
	 * builds on top of it. Server-only; false if none saved. The injector re-mints piece ids.
	 * Prefer PreferredFileName when non-empty and valid; else highest-ranked / most recent.
	 */
	bool PickCommunityArena(TArray<FPFBuildPieceRec>& OutPieces, const FString& PreferredFileName = FString()) const;

	/**
	 * Ranked community catalog for the map picker (top MaxCount, default 100).
	 * Host disk only (Saved/Arenas/*.json). Safe to call from the listen-host boot menu.
	 */
	void ListTopCommunityMaps(TArray<FPFCommunityMapInfo>& OutMaps, int32 MaxCount = 100) const;

	/** Load a specific Saved/Arenas file by filename (basename only). */
	bool LoadCommunityArenaByFileName(const FString& FileName, TArray<FPFBuildPieceRec>& OutPieces) const;

	/** Ensure starter community maps exist (first install / empty Arenas folder). */
	int32 EnsureSeedArenas() const;

private:
	/** One staged vote (AddVote input, held until CommitMatchRecord). Not a USTRUCT: no GC refs. */
	struct FPFPendingVote
	{
		FPFVoteRecord Vote;
		bool bBuiltHalfA = false;
	};

	/** True on listen server / standalone host; false on pure clients (no world = false). */
	bool IsServerContext() const;

	/** Loads + parses the most-recent Saved/Arenas/*.json into records. False if none/parse fail. */
	bool LoadMostRecentArena(TArray<FPFBuildPieceRec>& OutPieces) const;

	/** Parse one arena JSON file into map info + pieces; false if unusable. */
	bool ParseArenaFile(const FString& AbsolutePath, const FString& FileName,
	                    FPFCommunityMapInfo& OutInfo, TArray<FPFBuildPieceRec>& OutPieces) const;

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
