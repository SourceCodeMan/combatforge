// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFArenaSerialization.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/SecureHash.h"

namespace
{
	void AppendInt32LE(TArray<uint8>& Buffer, int32 Value)
	{
		const uint32 U = static_cast<uint32>(Value);
		Buffer.Add(static_cast<uint8>(U & 0xFF));
		Buffer.Add(static_cast<uint8>((U >> 8) & 0xFF));
		Buffer.Add(static_cast<uint8>((U >> 16) & 0xFF));
		Buffer.Add(static_cast<uint8>((U >> 24) & 0xFF));
	}

	void AppendInt16LE(TArray<uint8>& Buffer, int16 Value)
	{
		const uint16 U = static_cast<uint16>(Value);
		Buffer.Add(static_cast<uint8>(U & 0xFF));
		Buffer.Add(static_cast<uint8>((U >> 8) & 0xFF));
	}

	/** The canonical grid header — hashed ahead of the piece records (T27). */
	void AppendGridHeader(TArray<uint8>& Buffer)
	{
		AppendInt32LE(Buffer, PFGrid::CellUU);
		AppendInt32LE(Buffer, PFGrid::SubUU);
		AppendInt32LE(Buffer, PFGrid::WallHeightUU);
		AppendInt32LE(Buffer, PFGrid::CellsX);
		AppendInt32LE(Buffer, PFGrid::CellsY);
		AppendInt32LE(Buffer, PFGrid::Levels);
	}

	/** Deterministic canonical order: (Type, X, Y, Z, Rot, Team) — never PieceId/Owner. */
	void SortCanonical(TArray<FPFBuildPieceRec>& Recs)
	{
		Recs.Sort([](const FPFBuildPieceRec& A, const FPFBuildPieceRec& B)
		{
			if (A.Type != B.Type) { return A.Type < B.Type; }
			if (A.X != B.X)       { return A.X < B.X; }
			if (A.Y != B.Y)       { return A.Y < B.Y; }
			if (A.Z != B.Z)       { return A.Z < B.Z; }
			if (A.Rot != B.Rot)   { return A.Rot < B.Rot; }
			return A.Team < B.Team;
		});
	}

	constexpr int32 Sha1DigestBytes = 20;

	FString Sha1HexLower(const TArray<uint8>& Buffer)
	{
		uint8 Digest[Sha1DigestBytes] = { 0 };
		FSHA1 Sha;
		Sha.Update(Buffer.GetData(), Buffer.Num());
		Sha.Final();
		Sha.GetHash(Digest);

		FString Hex;
		Hex.Reserve(Sha1DigestBytes * 2);
		for (int32 ByteIdx = 0; ByteIdx < Sha1DigestBytes; ++ByteIdx)
		{
			Hex += FString::Printf(TEXT("%02x"), Digest[ByteIdx]);
		}
		return Hex;
	}
}

FString FPFArenaSerialization::ComputeArenaId(const TArray<FPFBuildPieceRec>& Pieces)
{
	TArray<FPFBuildPieceRec> Sorted = Pieces;
	SortCanonical(Sorted);

	TArray<uint8> Buffer;
	Buffer.Reserve(24 + Sorted.Num() * 9);
	AppendGridHeader(Buffer);
	for (const FPFBuildPieceRec& Rec : Sorted)
	{
		Buffer.Add(static_cast<uint8>(Rec.Type));
		AppendInt16LE(Buffer, Rec.X);
		AppendInt16LE(Buffer, Rec.Y);
		AppendInt16LE(Buffer, Rec.Z);
		Buffer.Add(Rec.Rot);
		Buffer.Add(Rec.Team);
	}
	return Sha1HexLower(Buffer);
}

FString FPFArenaSerialization::ComputeHalfHash(const TArray<FPFBuildPieceRec>& Pieces, uint8 Team)
{
	TArray<FPFBuildPieceRec> Sorted;
	Sorted.Reserve(Pieces.Num());
	for (const FPFBuildPieceRec& Rec : Pieces)
	{
		if (Rec.Team == Team)
		{
			Sorted.Add(Rec);
		}
	}
	SortCanonical(Sorted);

	TArray<uint8> Buffer;
	Buffer.Reserve(24 + Sorted.Num() * 8);
	AppendGridHeader(Buffer);
	for (const FPFBuildPieceRec& Rec : Sorted)
	{
		// Team excluded (T27): mirrored halves hash identically regardless of side.
		Buffer.Add(static_cast<uint8>(Rec.Type));
		AppendInt16LE(Buffer, Rec.X);
		AppendInt16LE(Buffer, Rec.Y);
		AppendInt16LE(Buffer, Rec.Z);
		Buffer.Add(Rec.Rot);
	}
	return Sha1HexLower(Buffer);
}

TSharedRef<FJsonObject> FPFArenaSerialization::BuildLayoutJson(const TArray<FPFBuildPieceRec>& Pieces,
                                                               const FString& MatchId, int32 TeamSize,
                                                               const FDateTime& CreatedUtc)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetStringField(TEXT("game"), TEXT("PaintForge"));
	Root->SetStringField(TEXT("matchId"), MatchId);
	// ISO-8601 Z with second precision, e.g. "2026-07-09T21:14:03Z" (§3.7 example).
	Root->SetStringField(TEXT("createdUtc"), FString::Printf(TEXT("%04d-%02d-%02dT%02d:%02d:%02dZ"),
		CreatedUtc.GetYear(), CreatedUtc.GetMonth(), CreatedUtc.GetDay(),
		CreatedUtc.GetHour(), CreatedUtc.GetMinute(), CreatedUtc.GetSecond()));
	Root->SetNumberField(TEXT("teamSize"), TeamSize);

	TSharedRef<FJsonObject> Grid = MakeShared<FJsonObject>();
	Grid->SetNumberField(TEXT("cellUU"), PFGrid::CellUU);
	Grid->SetNumberField(TEXT("subUU"), PFGrid::SubUU);
	Grid->SetNumberField(TEXT("wallH"), PFGrid::WallHeightUU);
	Grid->SetNumberField(TEXT("cellsX"), PFGrid::CellsX);
	Grid->SetNumberField(TEXT("cellsY"), PFGrid::CellsY);
	Grid->SetNumberField(TEXT("levels"), PFGrid::Levels);
	Root->SetObjectField(TEXT("grid"), Grid);

	Root->SetStringField(TEXT("arenaId"), ComputeArenaId(Pieces));
	Root->SetStringField(TEXT("halfHashA"), ComputeHalfHash(Pieces, 0));
	Root->SetStringField(TEXT("halfHashB"), ComputeHalfHash(Pieces, 1));

	TArray<TSharedPtr<FJsonValue>> PieceArray;
	PieceArray.Reserve(Pieces.Num());
	for (const FPFBuildPieceRec& Rec : Pieces)
	{
		TSharedRef<FJsonObject> PieceObj = MakeShared<FJsonObject>();   // mirrors FPFBuildPieceRec exactly
		PieceObj->SetNumberField(TEXT("id"), Rec.PieceId);
		PieceObj->SetNumberField(TEXT("t"), static_cast<int32>(Rec.Type));
		PieceObj->SetNumberField(TEXT("x"), Rec.X);
		PieceObj->SetNumberField(TEXT("y"), Rec.Y);
		PieceObj->SetNumberField(TEXT("z"), Rec.Z);
		PieceObj->SetNumberField(TEXT("r"), Rec.Rot);
		PieceObj->SetNumberField(TEXT("own"), Rec.OwnerIdx);
		PieceObj->SetNumberField(TEXT("team"), Rec.Team);
		PieceArray.Add(MakeShared<FJsonValueObject>(PieceObj));
	}
	Root->SetArrayField(TEXT("pieces"), PieceArray);

	return Root;
}

bool FPFArenaSerialization::ParseLayoutJson(const TSharedRef<FJsonObject>& Root,
	TArray<FPFBuildPieceRec>& OutPieces, int32& OutTeamSize)
{
	OutPieces.Reset();
	OutTeamSize = 0;
	Root->TryGetNumberField(TEXT("teamSize"), OutTeamSize);

	const TArray<TSharedPtr<FJsonValue>>* PieceArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("pieces"), PieceArray) || PieceArray == nullptr)
	{
		return false;
	}

	OutPieces.Reserve(PieceArray->Num());
	for (const TSharedPtr<FJsonValue>& Val : *PieceArray)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Val.IsValid() || !Val->TryGetObject(Obj) || Obj == nullptr || !(*Obj).IsValid())
		{
			continue;
		}
		int32 T = 0, X = 0, Y = 0, Z = 0, R = 0, Own = 0, Team = 0, Id = 0;
		(*Obj)->TryGetNumberField(TEXT("id"), Id);
		(*Obj)->TryGetNumberField(TEXT("t"), T);
		(*Obj)->TryGetNumberField(TEXT("x"), X);
		(*Obj)->TryGetNumberField(TEXT("y"), Y);
		(*Obj)->TryGetNumberField(TEXT("z"), Z);
		(*Obj)->TryGetNumberField(TEXT("r"), R);
		(*Obj)->TryGetNumberField(TEXT("own"), Own);
		(*Obj)->TryGetNumberField(TEXT("team"), Team);

		if (T < 0 || T >= static_cast<int32>(EPFPieceType::MAX_Count) || Team < 0 || Team > 1)
		{
			continue;   // skip corrupt records (would index the 14-ISM array out of range)
		}
		FPFBuildPieceRec Rec;
		Rec.PieceId  = static_cast<uint16>(Id);   // re-minted by the injector; not trusted here
		Rec.Type     = static_cast<EPFPieceType>(T);
		Rec.X        = static_cast<int16>(X);
		Rec.Y        = static_cast<int16>(Y);
		Rec.Z        = static_cast<int16>(Z);
		Rec.Rot      = static_cast<uint8>(R);
		Rec.OwnerIdx = static_cast<uint8>(Own);
		Rec.Team     = static_cast<uint8>(Team);
		OutPieces.Add(Rec);
	}
	return OutPieces.Num() > 0;
}
