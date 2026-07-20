// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Voting/PFRatingSubsystem.h"

#include "CombatForge.h"
#include "Core/PFPaths.h"
#include "Core/CombatForgeGameState.h"   // active ArenaMap → per-map grid rows (map-identity gate)
#include "Building/PFArenaSeed.h"
#include "Building/PFArenaSerialization.h"

#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UnrealClient.h"   // FScreenshotRequest (screenshot-on-publish)
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/** Build-grid rows of the map currently in play (Warehouse 10, Yard 20); default when no GameState yet. */
	int32 ResolveActiveGridCellsY(const UGameInstance* GI)
	{
		const UWorld* World = GI ? GI->GetWorld() : nullptr;
		const ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
		return GS ? PFGetArenaMapDef(GS->ArenaMap).CellsY : PFGrid::CellsY;
	}

	/** First 8 hex digits of the match GUID string, lowercased (dashes/braces skipped). */
	// CONTRACT-GAP: §3.7 says the filename uses the "matchId first 8 hex" without defining the
	// GUID string format; skipping non-hex separators and lowercasing is the smallest reading
	// that is stable across FGuid::ToString() formats.
	FString PFShortMatchHex(const FString& MatchId)
	{
		FString Out;
		Out.Reserve(8);
		for (const TCHAR Char : MatchId)
		{
			if (FChar::IsHexDigit(Char))
			{
				Out.AppendChar(FChar::ToLower(Char));
				if (Out.Len() == 8)
				{
					break;
				}
			}
		}
		return Out.IsEmpty() ? FString(TEXT("00000000")) : Out;
	}

	FString PFTeamLetter(uint8 Team)
	{
		return FString(Team == 0 ? TEXT("A") : TEXT("B"));
	}

	FString PFThumbString(EPFThumbVote Thumb)
	{
		switch (Thumb)
		{
		case EPFThumbVote::Up:   return FString(TEXT("up"));
		case EPFThumbVote::Down: return FString(TEXT("down"));
		default:                 return FString(TEXT("abstained"));
		}
	}

	/** Category IDs (1..8) → lowercase category-name JSON values (T13/T30); unknown ids dropped. */
	TArray<TSharedPtr<FJsonValue>> PFCategoryNameArray(const TArray<uint8>& Ids)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Ids.Num());
		for (const uint8 Id : Ids)
		{
			const FName Category = PFVoteCategories::FromId(Id);
			if (Category.IsNone())
			{
				UE_LOG(CombatForgeLog, Warning,
					TEXT("PFRatingSubsystem: unknown vote category id %u dropped from record."), Id);
				continue;
			}
			Out.Add(MakeShared<FJsonValueString>(Category.ToString()));
		}
		return Out;
	}
}

void UPFRatingSubsystem::BeginMatchRecord(const FString& MatchId,
                                          const TArray<FPFBuildPieceRec>& FrozenPieces,
                                          int32 TeamSize, const FString& ParentArenaId)
{
	if (!IsServerContext())
	{
		return;
	}

	if (bRecordActive)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("PFRatingSubsystem: BeginMatchRecord for match %s while record for match %s is still open — discarding the old record."),
			*MatchId, *CurrentMatchId);
		ClearRecordState();
	}

	// Empty arenas are not community content — writing them would pollute the Remix catalog with
	// unusable 0-piece files. Still clear any prior open record so state can't stick across matches.
	if (FrozenPieces.Num() == 0)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("PFRatingSubsystem: BeginMatchRecord skipped — 0 pieces (match %s). Remix needs builds."),
			*MatchId);
		ClearRecordState();
		return;
	}

	RecordCreatedUtc = FDateTime::UtcNow();
	CurrentMatchId   = MatchId;
	CurrentArenaId   = FPFArenaSerialization::ComputeArenaId(FrozenPieces);
	CurrentParentArenaId = ParentArenaId;   // BuildLayoutJson drops it when it equals CurrentArenaId (no real remix)
	CurrentRecordJson = FPFArenaSerialization::BuildLayoutJson(FrozenPieces, MatchId, TeamSize,
	                                                           RecordCreatedUtc,
	                                                           ResolveActiveGridCellsY(GetGameInstance()),
	                                                           ParentArenaId);

	const FString ArenaDir = FPFPaths::ArenaDir();   // stable dir (desktop UserSettings / server ProgramData)
	CurrentFilePath = ArenaDir / FString::Printf(TEXT("arena_%s_%s.json"),
		*RecordCreatedUtc.ToString(TEXT("%Y%m%d_%H%M%S")),
		*PFShortMatchHex(MatchId));

	PendingVotes.Reset();
	bRecordActive = true;

	if (WriteRecordToDisk())
	{
		const bool bRemix = !CurrentParentArenaId.IsEmpty() && CurrentParentArenaId != CurrentArenaId;
		UE_LOG(CombatForgeLog, Warning,
			TEXT("PFRatingSubsystem: SAVED map %s (arenaId %s, %d pieces%s) -> %s"),
			*CurrentMatchId, *CurrentArenaId, FrozenPieces.Num(),
			bRemix ? *FString::Printf(TEXT(", remix of %s"), *CurrentParentArenaId.Left(8)) : TEXT(""),
			*CurrentFilePath);
	}
}

void UPFRatingSubsystem::AddVote(const FPFVoteRecord& Vote, bool bBuiltHalfA)
{
	if (!IsServerContext())
	{
		return;
	}

	if (!bRecordActive)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("PFRatingSubsystem: AddVote with no active match record — vote dropped."));
		return;
	}

	// GameMode dedupes upstream (§3.7); if the same voter shows twice anyway, overwrite.
	if (!Vote.VoterGuidHash.IsEmpty())
	{
		for (FPFPendingVote& Existing : PendingVotes)
		{
			if (Existing.Vote.VoterGuidHash == Vote.VoterGuidHash)
			{
				UE_LOG(CombatForgeLog, Warning,
					TEXT("PFRatingSubsystem: duplicate vote from voter %s — overwriting staged vote."),
					*Vote.VoterGuidHash);
				Existing.Vote = Vote;
				Existing.bBuiltHalfA = bBuiltHalfA;
				return;
			}
		}
	}

	FPFPendingVote& Staged = PendingVotes.AddDefaulted_GetRef();
	Staged.Vote = Vote;
	Staged.bBuiltHalfA = bBuiltHalfA;

	UE_LOG(CombatForgeLog, Verbose,
		TEXT("PFRatingSubsystem: staged vote #%d for match %s (thumb %u, %d liked, %d disliked)."),
		PendingVotes.Num(), *CurrentMatchId, static_cast<uint32>(Vote.Thumb),
		Vote.LikedIds.Num(), Vote.DislikedIds.Num());
}

void UPFRatingSubsystem::CommitMatchRecord(const FPFMatchResult& Result)
{
	if (!IsServerContext())
	{
		return;
	}

	if (!bRecordActive || !CurrentRecordJson.IsValid())
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("PFRatingSubsystem: CommitMatchRecord with no active match record — ignored."));
		return;
	}

	// ---- ② result block (schema §3.7) ----
	const TSharedRef<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	FString WinnerString;
	if (Result.WinnerTeam == 0)
	{
		WinnerString = TEXT("A");
	}
	else if (Result.WinnerTeam == 1)
	{
		WinnerString = TEXT("B");
	}
	else
	{
		WinnerString = TEXT("draw");
	}
	ResultObj->SetStringField(TEXT("winnerTeam"), WinnerString);
	ResultObj->SetStringField(TEXT("finalScore"), FString::Printf(TEXT("%d-%d"),
		static_cast<int32>(Result.RoundWinsA), static_cast<int32>(Result.RoundWinsB)));
	ResultObj->SetNumberField(TEXT("roundsPlayed"), Result.RoundsPlayed);
	ResultObj->SetBoolField(TEXT("suddenDeath"), Result.bSuddenDeath);
	ResultObj->SetNumberField(TEXT("matchDurationSec"), Result.MatchDurationSec);
	CurrentRecordJson->SetObjectField(TEXT("result"), ResultObj);

	// ---- ③ votes block (one per staged vote; voterWonMatch resolved against the result) ----
	const bool bDraw = (Result.WinnerTeam != 0 && Result.WinnerTeam != 1);
	TArray<TSharedPtr<FJsonValue>> VotesArray;
	VotesArray.Reserve(PendingVotes.Num());
	for (const FPFPendingVote& Pending : PendingVotes)
	{
		const FPFVoteRecord& Vote = Pending.Vote;
		const TSharedRef<FJsonObject> VoteObj = MakeShared<FJsonObject>();
		VoteObj->SetStringField(TEXT("voterId"), Vote.VoterGuidHash);
		VoteObj->SetStringField(TEXT("voterTeam"), PFTeamLetter(Vote.VoterTeam));
		VoteObj->SetStringField(TEXT("voterBuiltHalf"), Pending.bBuiltHalfA ? TEXT("A") : TEXT("B"));
		VoteObj->SetBoolField(TEXT("voterWonMatch"),
			!bDraw && Vote.VoterTeam == Result.WinnerTeam);   // false for everyone on draw (B13)
		VoteObj->SetStringField(TEXT("thumb"), PFThumbString(Vote.Thumb));
		VoteObj->SetArrayField(TEXT("liked"), PFCategoryNameArray(Vote.LikedIds));
		VoteObj->SetArrayField(TEXT("disliked"), PFCategoryNameArray(Vote.DislikedIds));
		VotesArray.Add(MakeShared<FJsonValueObject>(VoteObj));
	}
	CurrentRecordJson->SetArrayField(TEXT("votes"), VotesArray);

	if (WriteRecordToDisk())
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("PFRatingSubsystem: committed match record %s (%s, %d votes) -> %s"),
			*CurrentMatchId, *WinnerString, VotesArray.Num(), *CurrentFilePath);
	}

	ClearRecordState();
}

FString UPFRatingSubsystem::GetCurrentArenaId() const
{
	return bRecordActive ? CurrentArenaId : FString();
}

bool UPFRatingSubsystem::ParseArenaFile(const FString& AbsolutePath, const FString& FileName,
	FPFCommunityMapInfo& OutInfo, TArray<FPFBuildPieceRec>& OutPieces) const
{
	OutInfo = FPFCommunityMapInfo();
	OutPieces.Reset();

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *AbsolutePath))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	int32 TeamSize = 0;
	if (!FPFArenaSerialization::ParseLayoutJson(Root.ToSharedRef(), OutPieces, TeamSize) || OutPieces.Num() == 0)
	{
		return false;
	}

	OutInfo.FileName = FileName;
	OutInfo.PieceCount = OutPieces.Num();
	OutInfo.TeamSize = TeamSize;
	// Build-grid rows this map was made on (default = Warehouse for legacy files with no grid block).
	OutInfo.CellsY = PFGrid::CellsY;
	const TSharedPtr<FJsonObject>* GridObj = nullptr;
	if (Root->TryGetObjectField(TEXT("grid"), GridObj) && GridObj != nullptr && (*GridObj).IsValid())
	{
		(*GridObj)->TryGetNumberField(TEXT("cellsY"), OutInfo.CellsY);
	}
	Root->TryGetStringField(TEXT("arenaId"), OutInfo.ArenaId);
	Root->TryGetStringField(TEXT("parentArenaId"), OutInfo.ParentArenaId);   // Remix lineage; absent on originals
	Root->TryGetStringField(TEXT("createdUtc"), OutInfo.CreatedUtc);

	// Vote tally → rank score (up +2, down −1).
	const TArray<TSharedPtr<FJsonValue>>* Votes = nullptr;
	if (Root->TryGetArrayField(TEXT("votes"), Votes) && Votes)
	{
		for (const TSharedPtr<FJsonValue>& Val : *Votes)
		{
			const TSharedPtr<FJsonObject>* VObj = nullptr;
			if (!Val.IsValid() || !Val->TryGetObject(VObj) || !VObj || !(*VObj).IsValid())
			{
				continue;
			}
			FString Thumb;
			(*VObj)->TryGetStringField(TEXT("thumb"), Thumb);
			if (Thumb.Equals(TEXT("up"), ESearchCase::IgnoreCase))
			{
				++OutInfo.ThumbUp;
				OutInfo.Score += 2;
			}
			else if (Thumb.Equals(TEXT("down"), ESearchCase::IgnoreCase))
			{
				++OutInfo.ThumbDown;
				OutInfo.Score -= 1;
			}
		}
	}
	// Soft boost for developed forts so empty vote records still rank by craft.
	OutInfo.Score += FMath::Clamp(OutInfo.PieceCount / 10, 0, 50);

	const FString ShortId = OutInfo.ArenaId.IsEmpty()
		? FileName.Left(12)
		: OutInfo.ArenaId.Left(8);
	const FString DatePart = OutInfo.CreatedUtc.IsEmpty()
		? TEXT("")
		: OutInfo.CreatedUtc.Left(10);
	OutInfo.DisplayName = FString::Printf(TEXT("%s  ·  %d pcs  ·  %+d"),
		DatePart.IsEmpty() ? *ShortId : *DatePart,
		OutInfo.PieceCount, OutInfo.Score);
	return true;
}

int32 UPFRatingSubsystem::EnsureSeedArenas() const
{
	const UGameInstance* GI = GetGameInstance();
	const UWorld* World = GI ? GI->GetWorld() : nullptr;
	if (World && World->GetNetMode() == NM_Client)
	{
		return 0;
	}
	return FPFArenaSeed::EnsureSeedArenas();
}

void UPFRatingSubsystem::ListTopCommunityMaps(TArray<FPFCommunityMapInfo>& OutMaps, int32 MaxCount) const
{
	OutMaps.Reset();
	// Catalog is host-local disk; pure clients get an empty list (host picks, GS replicates choice).
	const UGameInstance* GI = GetGameInstance();
	const UWorld* World = GI ? GI->GetWorld() : nullptr;
	if (World && World->GetNetMode() == NM_Client)
	{
		return;
	}

	// First-time / empty install: ship playable starter maps.
	EnsureSeedArenas();

	// Independent catalogs per arena shell (Tom): Warehouse maps never appear in the Yard picker
	// and vice versa. Same gate LoadCommunityArenaByFileName uses so list + load stay in lockstep.
	const int32 ActiveCY = ResolveActiveGridCellsY(GI);

	const FString Dir = FPFPaths::ArenaDir();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), /*Files=*/true, /*Directories=*/false);

	// arenaId → best entry (same fort played multiple matches → one picker row).
	TMap<FString, FPFCommunityMapInfo> BestByArena;
	for (const FString& FileName : Files)
	{
		FPFCommunityMapInfo Info;
		TArray<FPFBuildPieceRec> Pieces;
		if (!ParseArenaFile(Dir / FileName, FileName, Info, Pieces))
		{
			continue;
		}
		if (Info.CellsY != ActiveCY)
		{
			continue;   // other map's community builds stay on their own shell
		}
		const FString Key = Info.ArenaId.IsEmpty() ? FileName : Info.ArenaId;
		if (const FPFCommunityMapInfo* Existing = BestByArena.Find(Key))
		{
			if (Info.Score < Existing->Score
				|| (Info.Score == Existing->Score && Info.PieceCount < Existing->PieceCount))
			{
				continue;
			}
		}
		BestByArena.Add(Key, Info);
	}

	BestByArena.GenerateValueArray(OutMaps);
	OutMaps.Sort([](const FPFCommunityMapInfo& A, const FPFCommunityMapInfo& B)
	{
		if (A.Score != B.Score) { return A.Score > B.Score; }
		if (A.PieceCount != B.PieceCount) { return A.PieceCount > B.PieceCount; }
		return A.FileName > B.FileName;   // newer timestamp prefix sorts higher
	});

	const int32 Cap = FMath::Clamp(MaxCount, 1, 100);
	if (OutMaps.Num() > Cap)
	{
		OutMaps.SetNum(Cap);
	}
	UE_LOG(CombatForgeLog, Log, TEXT("RatingSubsystem: community map catalog %d for %d-row grid (cap %d)"),
		OutMaps.Num(), ActiveCY, Cap);
}

bool UPFRatingSubsystem::LoadCommunityArenaByFileName(const FString& FileName,
	TArray<FPFBuildPieceRec>& OutPieces) const
{
	OutPieces.Reset();
	if (FileName.IsEmpty() || FileName.Contains(TEXT("..")) || FileName.Contains(TEXT("/"))
		|| FileName.Contains(TEXT("\\")))
	{
		return false;   // path traversal guard — basename only
	}
	if (!FileName.EndsWith(TEXT(".json")))
	{
		return false;
	}
	const FString Path = FPFPaths::ArenaDir() / FileName;
	FPFCommunityMapInfo Info;
	if (!ParseArenaFile(Path, FileName, Info, OutPieces))
	{
		return false;
	}
	// Maps don't carry between grids (Tom 2026-07-17): a map built on a different grid — the Yard (20 rows)
	// vs the Warehouse (10) — can't be played on this shell. Reject so its pieces never land out-of-bounds.
	const int32 ActiveCY = ResolveActiveGridCellsY(GetGameInstance());
	if (Info.CellsY != ActiveCY)
	{
		UE_LOG(CombatForgeLog, Log,
			TEXT("Community map %s is a %d-row grid but the active map is %d rows — not loadable here."),
			*FileName, Info.CellsY, ActiveCY);
		OutPieces.Reset();
		return false;
	}
	return true;
}

bool UPFRatingSubsystem::LoadMostRecentArena(TArray<FPFBuildPieceRec>& OutPieces) const
{
	OutPieces.Reset();
	if (!IsServerContext())
	{
		return false;
	}
	// Prefer ranked catalog #1; fall back to newest non-empty file.
	TArray<FPFCommunityMapInfo> Top;
	ListTopCommunityMaps(Top, 1);
	if (Top.Num() > 0 && LoadCommunityArenaByFileName(Top[0].FileName, OutPieces))
	{
		UE_LOG(CombatForgeLog, Log, TEXT("RatingSubsystem: loaded top community arena %s (%d pieces)"),
			*Top[0].FileName, OutPieces.Num());
		return true;
	}

	const FString Dir = FPFPaths::ArenaDir();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), /*Files=*/true, /*Directories=*/false);
	if (Files.Num() == 0)
	{
		return false;
	}
	Files.Sort();
	for (int32 Idx = Files.Num() - 1; Idx >= 0; --Idx)
	{
		if (LoadCommunityArenaByFileName(Files[Idx], OutPieces))
		{
			UE_LOG(CombatForgeLog, Log, TEXT("RatingSubsystem: loaded community arena %s (%d pieces)"),
				*Files[Idx], OutPieces.Num());
			return true;
		}
	}
	return false;
}

bool UPFRatingSubsystem::PickCommunityArena(TArray<FPFBuildPieceRec>& OutPieces, FString& OutArenaId,
	const FString& PreferredFileName) const
{
	OutArenaId.Reset();

	const bool bLoaded =
		(!PreferredFileName.IsEmpty() && LoadCommunityArenaByFileName(PreferredFileName, OutPieces))
		|| LoadMostRecentArena(OutPieces);   // whole arena, both halves, unchanged
	if (!bLoaded)
	{
		return false;
	}

	// The Remix parent id is the CONTENT hash of the base layout — never a filename. It must be computed the
	// same way the child records its own arenaId so lineage lookups line up and the "unchanged ⇒ no parent"
	// guard in BuildLayoutJson fires correctly (a filename would never equal the child hash → false lineage).
	OutArenaId = FPFArenaSerialization::ComputeArenaId(OutPieces);
	return true;
}

bool UPFRatingSubsystem::PickCommunityHalf(TArray<FPFBuildPieceRec>& OutHalf, uint8 TargetTeam) const
{
	OutHalf.Reset();
	if (TargetTeam > 1)
	{
		return false;
	}
	TArray<FPFBuildPieceRec> All;
	if (!LoadMostRecentArena(All))
	{
		return false;
	}

	// Take the more-developed side (more pieces) and translate it into TargetTeam's plot.
	int32 CountA = 0, CountB = 0;
	for (const FPFBuildPieceRec& Rec : All)
	{
		(Rec.Team == 0 ? CountA : CountB)++;
	}
	const uint8 SourceHalf = (CountA >= CountB) ? 0 : 1;

	// The field is MIRROR-symmetric about its X-centre (team A faces +X toward the neutral strip, team B
	// faces -X). To move a source half to the other side FACING THE RIGHT WAY, reflect X about the centre
	// AND flip the +X/-X orientation — a pure translation would leave the fort backwards (cover against
	// its own spawn, open to the enemy). Reflection is X-only (the arena is symmetric in Y/Z). The X
	// offset differs by footprint: structural min-corner pieces occupy [X, X+4] → mirror = 60-X; an
	// E-edge wall's plane sits at X+4 → 56-X; props are centre-anchored → 64-X.
	const int32 FieldSubX = PFGrid::CellsX * PFGrid::SubPerCell;   // 64
	const int32 CellSub = PFGrid::SubPerCell;                      // 4
	auto ReflectRotX = [](uint8 Rot) -> uint8 { return (Rot == 0) ? 2 : (Rot == 2) ? 0 : Rot; };

	for (const FPFBuildPieceRec& Rec : All)
	{
		if (Rec.Team != SourceHalf)
		{
			continue;
		}
		FPFBuildPieceRec Out = Rec;
		if (SourceHalf != TargetTeam)
		{
			switch (Out.Type)
			{
			case EPFPieceType::Wall:
			case EPFPieceType::WallWindow:
			case EPFPieceType::WallDoor:
			case EPFPieceType::WallDoorOneWay:
				// Thin edges: E-edge (Rot 1) sits one cell further in X; wall rotation is preserved.
				// One-way front face flips with the half-mirror so the door still faces the mirrored approach.
				Out.X = static_cast<int16>((Out.Rot == 1) ? (FieldSubX - 2 * CellSub - Out.X)
				                                          : (FieldSubX - CellSub - Out.X));
				break;
			case EPFPieceType::Floor:
			case EPFPieceType::FloorTrap:
			case EPFPieceType::Ramp:
			case EPFPieceType::Roof:
				Out.X = static_cast<int16>(FieldSubX - CellSub - Out.X);
				Out.Rot = ReflectRotX(Out.Rot);   // ramp ascent +X <-> -X
				break;
			default:   // props are centre-anchored
				Out.X = static_cast<int16>(FieldSubX - Out.X);
				Out.Rot = ReflectRotX(Out.Rot);
				break;
			}
		}
		Out.Team = TargetTeam;
		OutHalf.Add(Out);
	}
	UE_LOG(CombatForgeLog, Log, TEXT("RatingSubsystem: remapped %d community pieces → team %d half"),
		OutHalf.Num(), TargetTeam);
	return OutHalf.Num() > 0;
}

bool UPFRatingSubsystem::IsServerContext() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	return World != nullptr && World->GetNetMode() != NM_Client;
}

bool UPFRatingSubsystem::WriteRecordToDisk() const
{
	if (!CurrentRecordJson.IsValid() || CurrentFilePath.IsEmpty())
	{
		return false;
	}

	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (!FJsonSerializer::Serialize(CurrentRecordJson.ToSharedRef(), Writer))
	{
		UE_LOG(CombatForgeLog, Error,
			TEXT("PFRatingSubsystem: failed to serialize match record for %s."), *CurrentFilePath);
		return false;
	}

	if (!FFileHelper::SaveStringToFile(Output, *CurrentFilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(CombatForgeLog, Error,
			TEXT("PFRatingSubsystem: failed to write match record file %s."), *CurrentFilePath);
		return false;
	}

	// Screenshot-on-publish (#6): capture the arena WITHOUT HUD next to its JSON so the community-map picker
	// can show a preview. Async (written a frame later); the arena is on screen during the post-match vote when
	// this runs. No-ops on a headless/dedicated host (no viewport) — those maps just stay preview-less.
	{
		FString ShotPath = CurrentFilePath;
		ShotPath.RemoveFromEnd(TEXT(".json"));
		ShotPath += TEXT(".png");
		FScreenshotRequest::RequestScreenshot(ShotPath, /*bInShowUI=*/false, /*bAddFilenameSuffix=*/false);
	}

	return true;
}

void UPFRatingSubsystem::ClearRecordState()
{
	CurrentRecordJson.Reset();
	PendingVotes.Reset();
	CurrentMatchId.Reset();
	CurrentArenaId.Reset();
	CurrentParentArenaId.Reset();
	CurrentFilePath.Reset();
	RecordCreatedUtc = FDateTime();
	bRecordActive = false;
}
