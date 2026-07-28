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

	/** The canonical grid header — hashed ahead of the piece records (T27). P2-BD1: rows + the
	 *  derived level count follow the ACTIVE map (same CellsY→Levels rule as BuildLayoutJson) so
	 *  same-piece layouts on different maps stop colliding on one arenaId. */
	void AppendGridHeader(TArray<uint8>& Buffer, int32 GridCellsY)
	{
		const int32 GridLevels = (GridCellsY >= PFGrid::MaxCellsY) ? PFGrid::YardLevels : PFGrid::Levels;
		AppendInt32LE(Buffer, PFGrid::CellUU);
		AppendInt32LE(Buffer, PFGrid::SubUU);
		AppendInt32LE(Buffer, PFGrid::WallHeightUU);
		AppendInt32LE(Buffer, PFGrid::CellsX);
		AppendInt32LE(Buffer, GridCellsY);
		AppendInt32LE(Buffer, GridLevels);
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

FString FPFArenaSerialization::ComputeArenaId(const TArray<FPFBuildPieceRec>& Pieces, int32 GridCellsY)
{
	TArray<FPFBuildPieceRec> Sorted = Pieces;
	SortCanonical(Sorted);

	TArray<uint8> Buffer;
	Buffer.Reserve(24 + Sorted.Num() * 9);
	AppendGridHeader(Buffer, GridCellsY);
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

FString FPFArenaSerialization::ComputeHalfHash(const TArray<FPFBuildPieceRec>& Pieces, uint8 Team,
	int32 GridCellsY)
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
	AppendGridHeader(Buffer, GridCellsY);
	for (const FPFBuildPieceRec& Rec : Sorted)
	{
		// Team excluded (T27) so the SAME half hashes the same whichever side owns it. Note this does NOT
		// make a team-B mirror equal its team-A original: X and Rot are still plot-absolute, so the two
		// halves of a mirrored arena hash differently. Transform into plot-local space first if a future
		// balance/lineage feature needs mirror equality. (P2-BD9)
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
                                                               const FDateTime& CreatedUtc,
                                                               int32 GridCellsY,
                                                               const FString& ParentArenaId)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetStringField(TEXT("game"), TEXT("CombatForge"));
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
	Grid->SetNumberField(TEXT("cellsY"), GridCellsY);   // per-map rows (Warehouse 10, Yard 20) — gates cross-map loads
	// Vertical stack follows the map: Warehouse 4 / Yard 7 (derived from row count so callers
	// that already pass GridCellsY don't need a second arg).
	const int32 GridLevels = (GridCellsY >= PFGrid::MaxCellsY) ? PFGrid::YardLevels : PFGrid::Levels;
	Grid->SetNumberField(TEXT("levels"), GridLevels);
	Root->SetObjectField(TEXT("grid"), Grid);

	const FString ArenaId = ComputeArenaId(Pieces, GridCellsY);
	Root->SetStringField(TEXT("arenaId"), ArenaId);
	Root->SetStringField(TEXT("halfHashA"), ComputeHalfHash(Pieces, 0, GridCellsY));
	Root->SetStringField(TEXT("halfHashB"), ComputeHalfHash(Pieces, 1, GridCellsY));

	// Remix lineage: record the source map ONLY when this is a genuine fork (the layout actually changed).
	// Equal ids ⇒ nothing was remixed (or a Play-Only replay) ⇒ no parent; empty ⇒ Creative/from scratch.
	if (!ParentArenaId.IsEmpty() && ParentArenaId != ArenaId)
	{
		Root->SetStringField(TEXT("parentArenaId"), ParentArenaId);
	}

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

	// Grid-header guard: a file saved against different grid dims would silently land its pieces on the
	// wrong grid. CellUU/CellsX are frozen on every map; CellsY is per-map — the two valid grids are the
	// Warehouse (10) and the Yard (20). Accept either here (so a Yard map parses for the catalog); the
	// cross-map "can't play a Yard map on the Warehouse" gate is enforced at load (LoadCommunityArenaByFileName,
	// against the ACTIVE map). Anything else is a foreign/corrupt file → reject. No grid block = legacy, accepted.
	const TSharedPtr<FJsonObject>* GridObj = nullptr;
	if (Root->TryGetObjectField(TEXT("grid"), GridObj) && GridObj != nullptr && (*GridObj).IsValid())
	{
		int32 CellUU = PFGrid::CellUU, CellsX = PFGrid::CellsX, CellsY = PFGrid::CellsY;
		int32 Levels = PFGrid::Levels;
		(*GridObj)->TryGetNumberField(TEXT("cellUU"), CellUU);
		(*GridObj)->TryGetNumberField(TEXT("cellsX"), CellsX);
		(*GridObj)->TryGetNumberField(TEXT("cellsY"), CellsY);
		(*GridObj)->TryGetNumberField(TEXT("levels"), Levels);
		const bool bKnownRows = (CellsY == PFGrid::CellsY || CellsY == PFGrid::MaxCellsY);
		// Accept Warehouse (4) or Yard (7) level counts; missing/legacy levels defaults above to 4.
		const bool bKnownLevels = (Levels == PFGrid::Levels || Levels == PFGrid::YardLevels);
		if (CellUU != PFGrid::CellUU || CellsX != PFGrid::CellsX || !bKnownRows || !bKnownLevels)
		{
			return false;
		}
	}

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

		const bool bWallLike = (T == static_cast<int32>(EPFPieceType::Wall)
			|| T == static_cast<int32>(EPFPieceType::WallWindow)
			|| T == static_cast<int32>(EPFPieceType::WallDoor)
			|| T == static_cast<int32>(EPFPieceType::WallDoorOneWay));
		const int32 MaxRot = bWallLike ? 1 : 3;   // wall-like: N/E only
		if (T < 0 || T >= static_cast<int32>(EPFPieceType::MAX_Count) || Team < 0 || Team > 1
			|| R < 0 || R > MaxRot
			|| X < -1000 || X > 1000 || Y < -1000 || Y > 1000 || Z < -1000 || Z > 1000)
		{
			// Reject corrupt records: a bad type/team would index the 14-ISM array out of range; a bad
			// Rot aliases occupancy keys; wild coords would silently wrap the int16 cast below.
			continue;
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
