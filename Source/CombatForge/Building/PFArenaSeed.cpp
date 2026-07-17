// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFArenaSeed.h"

#include "CombatForge.h"
#include "Core/PFPaths.h"
#include "Building/PFArenaSerialization.h"
#include "Core/CombatForgeTypes.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FPFBuildPieceRec MakePiece(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot, uint8 Team)
	{
		FPFBuildPieceRec Rec;
		Rec.PieceId = 0;   // re-minted on inject
		Rec.Type = Type;
		Rec.X = X;
		Rec.Y = Y;
		Rec.Z = Z;
		Rec.Rot = Rot;
		Rec.OwnerIdx = 255;
		Rec.Team = Team;
		return Rec;
	}

	/** Symmetric mirror of a team-0 structural/prop layout onto team 1. */
	void MirrorTeamAToB(const TArray<FPFBuildPieceRec>& A, TArray<FPFBuildPieceRec>& Out)
	{
		const int32 FieldSubX = PFGrid::CellsX * PFGrid::SubPerCell; // 64
		const int32 CellSub = PFGrid::SubPerCell;                    // 4
		auto ReflectRotX = [](uint8 Rot) -> uint8
		{
			return (Rot == 0) ? 2 : (Rot == 2) ? 0 : Rot;
		};

		for (const FPFBuildPieceRec& In : A)
		{
			FPFBuildPieceRec OutRec = In;
			OutRec.Team = 1;
			switch (OutRec.Type)
			{
			case EPFPieceType::Wall:
			case EPFPieceType::WallWindow:
			case EPFPieceType::WallDoor:
			case EPFPieceType::WallDoorOneWay:
				OutRec.X = static_cast<int16>((OutRec.Rot == 1)
					? (FieldSubX - 2 * CellSub - OutRec.X)
					: (FieldSubX - CellSub - OutRec.X));
				break;
			case EPFPieceType::Floor:
			case EPFPieceType::FloorTrap:
			case EPFPieceType::Ramp:
			case EPFPieceType::Roof:
				OutRec.X = static_cast<int16>(FieldSubX - CellSub - OutRec.X);
				OutRec.Rot = ReflectRotX(OutRec.Rot);
				break;
			default:
				OutRec.X = static_cast<int16>(FieldSubX - OutRec.X);
				OutRec.Rot = ReflectRotX(OutRec.Rot);
				break;
			}
			Out.Add(OutRec);
		}
	}

	bool WriteSeedFile(const FString& FileName, const FString& MatchIdTag,
		const TArray<FPFBuildPieceRec>& Pieces)
	{
		const FString Dir = FPFPaths::ArenaDir();   // stable per-user dir (survives repackaging)
		const FString Path = Dir / FileName;
		if (IFileManager::Get().FileExists(*Path))
		{
			return false;   // already present
		}

		const FDateTime Utc = FDateTime::UtcNow();
		const TSharedRef<FJsonObject> Root = FPFArenaSerialization::BuildLayoutJson(
			Pieces, MatchIdTag, /*TeamSize=*/4, Utc, /*GridCellsY=*/PFGrid::CellsY);   // starter seeds are Warehouse-grid

		// Seed votes so ranking prefers these over empty dumps.
		TArray<TSharedPtr<FJsonValue>> Votes;
		{
			TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
			V->SetStringField(TEXT("voterId"), TEXT("seed"));
			V->SetStringField(TEXT("voterTeam"), TEXT("A"));
			V->SetStringField(TEXT("voterBuiltHalf"), TEXT("A"));
			V->SetBoolField(TEXT("voterWonMatch"), true);
			V->SetStringField(TEXT("thumb"), TEXT("up"));
			V->SetArrayField(TEXT("liked"), TArray<TSharedPtr<FJsonValue>>());
			V->SetArrayField(TEXT("disliked"), TArray<TSharedPtr<FJsonValue>>());
			Votes.Add(MakeShared<FJsonValueObject>(V));
		}
		{
			TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
			V->SetStringField(TEXT("voterId"), TEXT("seed2"));
			V->SetStringField(TEXT("voterTeam"), TEXT("B"));
			V->SetStringField(TEXT("voterBuiltHalf"), TEXT("B"));
			V->SetBoolField(TEXT("voterWonMatch"), false);
			V->SetStringField(TEXT("thumb"), TEXT("up"));
			V->SetArrayField(TEXT("liked"), TArray<TSharedPtr<FJsonValue>>());
			V->SetArrayField(TEXT("disliked"), TArray<TSharedPtr<FJsonValue>>());
			Votes.Add(MakeShared<FJsonValueObject>(V));
		}
		Root->SetArrayField(TEXT("votes"), Votes);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			return false;
		}
		if (!FFileHelper::SaveStringToFile(Out, *Path))
		{
			UE_LOG(CombatForgeLog, Error, TEXT("ArenaSeed: failed to write %s"), *Path);
			return false;
		}
		UE_LOG(CombatForgeLog, Log, TEXT("ArenaSeed: wrote %s (%d pieces)"), *FileName, Pieces.Num());
		return true;
	}

	/** Lane cover: forward walls + floors + barrels — simple first-game fort. */
	TArray<FPFBuildPieceRec> BuildStarterLanes()
	{
		TArray<FPFBuildPieceRec> A;
		// Team 0 plot cells X 1..6 → sub X 4..24. Midline faces +X (east).
		// Front wall line near plot edge (cell X=5 → sub 20), Y span cover.
		for (int32 Y = 8; Y <= 28; Y += 4)
		{
			A.Add(MakePiece(EPFPieceType::Wall, 20, static_cast<int16>(Y), 0, /*Rot E*/1, 0));
			A.Add(MakePiece(EPFPieceType::Floor, 16, static_cast<int16>(Y), 0, 0, 0));
		}
		// Side flanks
		A.Add(MakePiece(EPFPieceType::Wall, 12, 8, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::Wall, 12, 28, 0, 0, 0));
		// Barrels as soft cover
		A.Add(MakePiece(EPFPieceType::PropCan, 14, 14, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::PropCan, 14, 22, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::PropDorito, 18, 18, 0, 0, 0));

		TArray<FPFBuildPieceRec> All = A;
		MirrorTeamAToB(A, All);
		return All;
	}

	/** Heavier bunker: dual wall lines + ramps + props. */
	TArray<FPFBuildPieceRec> BuildBunkerBoxes()
	{
		TArray<FPFBuildPieceRec> A;
		for (int32 Y = 4; Y <= 32; Y += 4)
		{
			A.Add(MakePiece(EPFPieceType::Wall, 20, static_cast<int16>(Y), 0, 1, 0));
			A.Add(MakePiece(EPFPieceType::Wall, 16, static_cast<int16>(Y), 0, 1, 0));
			A.Add(MakePiece(EPFPieceType::Floor, 12, static_cast<int16>(Y), 0, 0, 0));
			A.Add(MakePiece(EPFPieceType::Floor, 16, static_cast<int16>(Y), 0, 0, 0));
		}
		// Second level platform
		for (int32 Y = 12; Y <= 24; Y += 4)
		{
			A.Add(MakePiece(EPFPieceType::Floor, 12, static_cast<int16>(Y), 3, 0, 0));
			A.Add(MakePiece(EPFPieceType::Wall, 16, static_cast<int16>(Y), 3, 1, 0));
		}
		A.Add(MakePiece(EPFPieceType::Ramp, 12, 8, 0, /*+X*/0, 0));
		A.Add(MakePiece(EPFPieceType::PropSnake, 18, 12, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::PropSnake, 18, 24, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::PropCan, 14, 18, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::PropDorito, 18, 18, 0, 0, 0));

		TArray<FPFBuildPieceRec> All = A;
		MirrorTeamAToB(A, All);
		return All;
	}

	/** Vertical play: ramps to level 1 roofs/floors. */
	TArray<FPFBuildPieceRec> BuildRampsHeights()
	{
		TArray<FPFBuildPieceRec> A;
		for (int32 Y = 8; Y <= 28; Y += 4)
		{
			A.Add(MakePiece(EPFPieceType::Floor, 8, static_cast<int16>(Y), 0, 0, 0));
			A.Add(MakePiece(EPFPieceType::Floor, 12, static_cast<int16>(Y), 0, 0, 0));
			A.Add(MakePiece(EPFPieceType::Floor, 12, static_cast<int16>(Y), 3, 0, 0));
			A.Add(MakePiece(EPFPieceType::Wall, 16, static_cast<int16>(Y), 0, 1, 0));
			A.Add(MakePiece(EPFPieceType::Wall, 16, static_cast<int16>(Y), 3, 1, 0));
		}
		A.Add(MakePiece(EPFPieceType::Ramp, 8, 12, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::Ramp, 8, 20, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::Roof, 12, 16, 3, 0, 0));
		A.Add(MakePiece(EPFPieceType::PropCan, 14, 10, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::PropCan, 14, 26, 0, 0, 0));
		A.Add(MakePiece(EPFPieceType::PropDorito, 10, 18, 3, 0, 0));

		TArray<FPFBuildPieceRec> All = A;
		MirrorTeamAToB(A, All);
		return All;
	}
}

int32 FPFArenaSeed::EnsureSeedArenas()
{
	int32 Written = 0;
	if (WriteSeedFile(TEXT("seed_starter_lanes.json"), TEXT("seed-starter-lanes"), BuildStarterLanes()))
	{
		++Written;
	}
	if (WriteSeedFile(TEXT("seed_bunker_boxes.json"), TEXT("seed-bunker-boxes"), BuildBunkerBoxes()))
	{
		++Written;
	}
	if (WriteSeedFile(TEXT("seed_ramps_heights.json"), TEXT("seed-ramps-heights"), BuildRampsHeights()))
	{
		++Written;
	}
	if (Written > 0)
	{
		UE_LOG(CombatForgeLog, Log, TEXT("ArenaSeed: created %d seed community map(s)"), Written);
	}
	return Written;
}
