// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Voting/PFRatingSubsystem.h"

#include "PaintForge.h"
#include "Building/PFArenaSerialization.h"

#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
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
				UE_LOG(PaintForgeLog, Warning,
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
                                          int32 TeamSize)
{
	if (!IsServerContext())
	{
		return;
	}

	if (bRecordActive)
	{
		UE_LOG(PaintForgeLog, Warning,
			TEXT("PFRatingSubsystem: BeginMatchRecord for match %s while record for match %s is still open — discarding the old record."),
			*MatchId, *CurrentMatchId);
		ClearRecordState();
	}

	RecordCreatedUtc = FDateTime::UtcNow();
	CurrentMatchId   = MatchId;
	CurrentArenaId   = FPFArenaSerialization::ComputeArenaId(FrozenPieces);
	CurrentRecordJson = FPFArenaSerialization::BuildLayoutJson(FrozenPieces, MatchId, TeamSize,
	                                                           RecordCreatedUtc);

	const FString ArenaDir = FPaths::ProjectSavedDir() / TEXT("Arenas");
	IFileManager::Get().MakeDirectory(*ArenaDir, /*Tree=*/true);
	CurrentFilePath = ArenaDir / FString::Printf(TEXT("arena_%s_%s.json"),
		*RecordCreatedUtc.ToString(TEXT("%Y%m%d_%H%M%S")),
		*PFShortMatchHex(MatchId));

	PendingVotes.Reset();
	bRecordActive = true;

	if (WriteRecordToDisk())
	{
		UE_LOG(PaintForgeLog, Log,
			TEXT("PFRatingSubsystem: began match record %s (arenaId %s, %d pieces) -> %s"),
			*CurrentMatchId, *CurrentArenaId, FrozenPieces.Num(), *CurrentFilePath);
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
		UE_LOG(PaintForgeLog, Warning,
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
				UE_LOG(PaintForgeLog, Warning,
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

	UE_LOG(PaintForgeLog, Verbose,
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
		UE_LOG(PaintForgeLog, Warning,
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
		UE_LOG(PaintForgeLog, Log,
			TEXT("PFRatingSubsystem: committed match record %s (%s, %d votes) -> %s"),
			*CurrentMatchId, *WinnerString, VotesArray.Num(), *CurrentFilePath);
	}

	ClearRecordState();
}

FString UPFRatingSubsystem::GetCurrentArenaId() const
{
	return bRecordActive ? CurrentArenaId : FString();
}

bool UPFRatingSubsystem::PickCommunityHalf(TArray<FPFBuildPieceRec>& OutHalf, uint8 TargetTeam) const
{
	OutHalf.Reset();
	if (!IsServerContext() || TargetTeam > 1)
	{
		return false;
	}

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("Arenas");
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), /*Files=*/true, /*Directories=*/false);
	if (Files.Num() == 0)
	{
		return false;   // no community arenas saved yet
	}
	// v1: the most-recent file (names are timestamp-sorted). Vote-ranked selection can refine this later.
	Files.Sort();
	const FString ChosenPath = Dir / Files.Last();

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *ChosenPath))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(PaintForgeLog, Warning, TEXT("RatingSubsystem: could not parse arena %s"), *ChosenPath);
		return false;
	}

	TArray<FPFBuildPieceRec> All;
	int32 TeamSize = 0;
	if (!FPFArenaSerialization::ParseLayoutJson(Root.ToSharedRef(), All, TeamSize))
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
	// Plot A = cells 1..6, Plot B = cells 9..14 → 8 cells apart = 32 sub-grid units in X (PFGrid).
	const int16 PlotDeltaSub = 32;

	for (const FPFBuildPieceRec& Rec : All)
	{
		if (Rec.Team != SourceHalf)
		{
			continue;
		}
		FPFBuildPieceRec Out = Rec;
		if (SourceHalf != TargetTeam)
		{
			Out.X += (TargetTeam == 1) ? PlotDeltaSub : static_cast<int16>(-PlotDeltaSub);
		}
		Out.Team = TargetTeam;
		OutHalf.Add(Out);
	}
	UE_LOG(PaintForgeLog, Log, TEXT("RatingSubsystem: picked community half from %s (%d pieces → team %d)"),
		*Files.Last(), OutHalf.Num(), TargetTeam);
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
		UE_LOG(PaintForgeLog, Error,
			TEXT("PFRatingSubsystem: failed to serialize match record for %s."), *CurrentFilePath);
		return false;
	}

	if (!FFileHelper::SaveStringToFile(Output, *CurrentFilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(PaintForgeLog, Error,
			TEXT("PFRatingSubsystem: failed to write match record file %s."), *CurrentFilePath);
		return false;
	}

	return true;
}

void UPFRatingSubsystem::ClearRecordState()
{
	CurrentRecordJson.Reset();
	PendingVotes.Reset();
	CurrentMatchId.Reset();
	CurrentArenaId.Reset();
	CurrentFilePath.Reset();
	RecordCreatedUtc = FDateTime();
	bRecordActive = false;
}
