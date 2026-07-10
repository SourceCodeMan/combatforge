// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFBuildGrid.h"

#include "PaintForge.h"
#include "Building/PFGridMath.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	const TCHAR* PFPieceTypeName(EPFPieceType Type)
	{
		switch (Type)
		{
		case EPFPieceType::Wall:       return TEXT("Wall");
		case EPFPieceType::Floor:      return TEXT("Floor");
		case EPFPieceType::Ramp:       return TEXT("Ramp");
		case EPFPieceType::Roof:       return TEXT("Roof");
		case EPFPieceType::PropCan:    return TEXT("Can");
		case EPFPieceType::PropDorito: return TEXT("Dorito");
		case EPFPieceType::PropSnake:  return TEXT("Snake");
		default:                       return TEXT("Invalid");
		}
	}
}

// ---------------------------------------------------------------------------
// FPFBuildPieceArray — client mirror hooks (§5.11: visuals ONLY from these)
// ---------------------------------------------------------------------------

void FPFBuildPieceArray::PostReplicatedAdd(const TArrayView<int32>& AddedIndices, int32 /*FinalSize*/)
{
	if (!OwnerGrid)
	{
		return;
	}
	for (const int32 Idx : AddedIndices)
	{
		if (Items.IsValidIndex(Idx))
		{
			OwnerGrid->AddPieceLocal(Items[Idx]);
		}
	}
}

void FPFBuildPieceArray::PreReplicatedRemove(const TArrayView<int32>& RemovedIndices, int32 /*FinalSize*/)
{
	if (!OwnerGrid)
	{
		return;
	}
	for (const int32 Idx : RemovedIndices)
	{
		if (Items.IsValidIndex(Idx))
		{
			OwnerGrid->RemovePieceLocal(Items[Idx]);
		}
	}
}

void FPFBuildPieceArray::PostReplicatedChange(const TArrayView<int32>& /*ChangedIndices*/, int32 /*FinalSize*/)
{
	// Piece records are immutable in v1 — nothing to do.
}

// ---------------------------------------------------------------------------
// APFBuildGrid
// ---------------------------------------------------------------------------

APFBuildGrid::APFBuildGrid()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(10.f);   // T28

	Pieces.OwnerGrid = this;

	GridRoot = CreateDefaultSubobject<USceneComponent>(TEXT("GridRoot"));
	SetRootComponent(GridRoot);
	GridRoot->SetMobility(EComponentMobility::Static);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UMaterial>   MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	ShapeMaterial = MaterialFinder.Object;

	UStaticMesh* MeshPerType[7] =
	{
		CubeFinder.Object,       // Wall
		CubeFinder.Object,       // Floor
		CubeFinder.Object,       // Ramp (plank)
		ConeFinder.Object,       // Roof (cone proxy)
		CylinderFinder.Object,   // Can
		ConeFinder.Object,       // Dorito
		CubeFinder.Object        // Snake
	};

	for (int32 TypeIdx = 0; TypeIdx < 7; ++TypeIdx)
	{
		for (uint8 Team = 0; Team < 2; ++Team)
		{
			const int32 K = ISMCIndexFor(static_cast<EPFPieceType>(TypeIdx), Team);
			const FName CompName(*FString::Printf(TEXT("ISM_%s_Team%d"),
				PFPieceTypeName(static_cast<EPFPieceType>(TypeIdx)), Team));

			UInstancedStaticMeshComponent* ISMC = CreateDefaultSubobject<UInstancedStaticMeshComponent>(CompName);
			ISMC->SetupAttachment(GridRoot);
			ISMC->SetMobility(EComponentMobility::Static);
			ISMC->SetStaticMesh(MeshPerType[TypeIdx]);
			ISMC->SetMaterial(0, ShapeMaterial);
			ISMC->SetCanEverAffectNavigation(false);
			ISMC->SetCastShadow(true);

			// Block Pawn / Visibility / Paintball / BuildTrace (§4.6); ignore everything else.
			ISMC->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			ISMC->SetCollisionObjectType(ECC_WorldStatic);
			ISMC->SetCollisionResponseToAllChannels(ECR_Ignore);
			ISMC->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
			ISMC->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
			ISMC->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
			ISMC->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
			ISMC->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
			ISMC->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Block);

			PieceISMCs[K] = ISMC;
		}
	}
}

void APFBuildGrid::BeginPlay()
{
	Super::BeginPlay();

	Pieces.OwnerGrid = this;   // belt+braces: ctor set it, keep it correct post-init on both sides

	// Team-tinted MIDs (T8): BasicShapeMaterial's "Color" vector param is the one tint knob.
	for (int32 TypeIdx = 0; TypeIdx < 7; ++TypeIdx)
	{
		for (uint8 Team = 0; Team < 2; ++Team)
		{
			const int32 K = ISMCIndexFor(static_cast<EPFPieceType>(TypeIdx), Team);
			if (PieceISMCs[K] && ShapeMaterial)
			{
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ShapeMaterial, this);
				MID->SetVectorParameterValue(TEXT("Color"), PFColors::ForTeam(Team));
				PieceISMCs[K]->SetMaterial(0, MID);
				TeamMIDs[K] = MID;
			}
		}
	}
}

void APFBuildGrid::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APFBuildGrid, Pieces);
}

int32 APFBuildGrid::ISMCIndexFor(EPFPieceType Type, uint8 Team)
{
	return static_cast<int32>(Type) * 2 + ((Team == 1) ? 1 : 0);
}

FIntVector APFBuildGrid::WallEdgeKey(int32 Cx, int32 Cy, int32 Level, uint8 EdgeNE)
{
	return FIntVector(Cx, Cy, Level * 2 + static_cast<int32>(EdgeNE));
}

// ---------------------------------------------------------------------------
// Shared validation
// ---------------------------------------------------------------------------

EPFDenyReason APFBuildGrid::QueryPlacement(const FPFPlacementQuery& Q) const
{
	// --- Piece sanity ---
	if (Q.Type >= EPFPieceType::MAX_Count)
	{
		return EPFDenyReason::InvalidPiece;
	}
	const bool bProp = PFIsProp(Q.Type);
	if (Q.Type == EPFPieceType::Wall ? (Q.Rot > 1) : (Q.Rot > 3))
	{
		return EPFDenyReason::InvalidPiece;
	}
	if (Q.Z < 0 || (Q.Z % 3) != 0)   // structural level*3; prop support tops are 300-multiples (T25)
	{
		return EPFDenyReason::InvalidPiece;
	}
	if (!bProp && ((Q.X % PFGrid::SubPerCell) != 0 || (Q.Y % PFGrid::SubPerCell) != 0))
	{
		return EPFDenyReason::InvalidPiece;
	}

	// --- Phase gate (server truth; the client runs the same predicate for ghost tint) ---
	if (bBuildFrozen)
	{
		return EPFDenyReason::WrongPhase;
	}
	const UWorld* World = GetWorld();
	const APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (!GS || !GS->IsBuildAllowed())
	{
		return EPFDenyReason::WrongPhase;
	}

	// --- Bounds ---
	FBox Bounds(ForceInit);
	if (!FPFGridMath::PieceAABB(Q.Type, Q.X, Q.Y, Q.Z, Q.Rot, Bounds))
	{
		return EPFDenyReason::InvalidPiece;
	}

	// --- Plot ownership (also excludes spawn columns + neutral strip by construction) ---
	if (Q.Team > 1)
	{
		return EPFDenyReason::OutOfPlot;
	}
	if (Q.Type == EPFPieceType::Wall)
	{
		// A canonical edge belongs to a team if either adjacent cell is in its plot — keeps the
		// front-line edges (plot|neutral) symmetric for both teams.
		const int32 Cx = Q.X / PFGrid::SubPerCell;
		const int32 Cy = Q.Y / PFGrid::SubPerCell;
		const int32 C2x = (Q.Rot == 1) ? Cx + 1 : Cx;
		const int32 C2y = (Q.Rot == 0) ? Cy + 1 : Cy;
		if (!FPFGridMath::IsCellInTeamPlot(Cx, Cy, Q.Team) && !FPFGridMath::IsCellInTeamPlot(C2x, C2y, Q.Team))
		{
			return EPFDenyReason::OutOfPlot;
		}
	}
	else if (!bProp)
	{
		if (!FPFGridMath::IsInsideTeamPlot(Q.Type, Q.X, Q.Y, Q.Team))
		{
			return EPFDenyReason::OutOfPlot;
		}
	}
	else
	{
		// Props: rot-aware AABB fully inside the plot rectangle.
		int32 MinC = 0, MaxC = 0;
		if (!FPFGridMath::TeamPlotColumns(Q.Team, MinC, MaxC))
		{
			return EPFDenyReason::OutOfPlot;
		}
		constexpr float Eps = 0.5f;
		const float PlotMinX = MinC * PFGrid::CellUU;
		const float PlotMaxX = (MaxC + 1) * PFGrid::CellUU;
		const float PlotMaxY = static_cast<float>(PFGrid::CellsY * PFGrid::CellUU);
		if (Bounds.Min.X < PlotMinX - Eps || Bounds.Max.X > PlotMaxX + Eps ||
			Bounds.Min.Y < -Eps || Bounds.Max.Y > PlotMaxY + Eps)
		{
			return EPFDenyReason::OutOfPlot;
		}
	}

	// --- Height cap (walls: level 0..2 only — a level-3-based wall breaches the cap) ---
	const int32 Level = Q.Z / 3;
	if (Q.Type == EPFPieceType::Wall && Level > PFGrid::Levels - 2)
	{
		return EPFDenyReason::HeightCap;
	}
	if (!bProp && Level > PFGrid::Levels - 1)
	{
		return EPFDenyReason::HeightCap;
	}
	if (Bounds.Max.Z > static_cast<float>(PFGrid::HeightCapUU) + 0.5f)
	{
		return EPFDenyReason::HeightCap;
	}

	// --- Occupancy (structural) / overlap (props) ---
	if (!bProp)
	{
		const int32 Cx = Q.X / PFGrid::SubPerCell;
		const int32 Cy = Q.Y / PFGrid::SubPerCell;
		switch (Q.Type)
		{
		case EPFPieceType::Wall:
			if (WallEdges.Contains(WallEdgeKey(Cx, Cy, Level, Q.Rot)))
			{
				return EPFDenyReason::SlotOccupied;
			}
			break;
		case EPFPieceType::Floor:
			if (FloorSlots.Contains(FIntVector(Cx, Cy, Level)))
			{
				return EPFDenyReason::SlotOccupied;
			}
			break;
		case EPFPieceType::Ramp:
		case EPFPieceType::Roof:
			if (InclineSlots.Contains(FIntVector(Cx, Cy, Level)))   // one incline per cell-level (ramp XOR roof)
			{
				return EPFDenyReason::SlotOccupied;
			}
			break;
		default:
			break;
		}
	}
	else
	{
		// Shrink slightly so flush contact (snake chains, props against walls) is legal.
		const FBox Shrunk = Bounds.ExpandBy(-1.f);
		for (const TPair<uint16, FBox>& Existing : StructuralBounds)
		{
			if (Shrunk.Intersect(Existing.Value))
			{
				return EPFDenyReason::Overlapping;
			}
		}
		for (const TPair<uint16, FBox>& Existing : PropBounds)
		{
			if (Shrunk.Intersect(Existing.Value))
			{
				return EPFDenyReason::Overlapping;
			}
		}
	}

	// --- Anchor rule ---
	if (!HasAnchor(Q, Bounds))
	{
		return EPFDenyReason::NoAnchor;
	}

	return EPFDenyReason::None;
}

bool APFBuildGrid::HasAnchor(const FPFPlacementQuery& Q, const FBox& Bounds) const
{
	// Terrain: level 0 always qualifies (props: resting on the field floor at Z = 0).
	if (Q.Z == 0)
	{
		return true;
	}
	// Otherwise the bounds must touch an existing structural piece (any team's — moot in
	// practice since plots don't overlap). No collapse propagation: floaters are legal (03 §4).
	const FBox Test = Bounds.ExpandBy(2.f);
	for (const TPair<uint16, FBox>& Structural : StructuralBounds)
	{
		if (Test.Intersect(Structural.Value))
		{
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Server-only mutation
// ---------------------------------------------------------------------------

EPFDenyReason APFBuildGrid::TryPlacePiece(APaintForgePlayerState* Placer, const FPFPlacementQuery& Q, uint16& OutPieceId)
{
	OutPieceId = 0;
	if (!HasAuthority())
	{
		return EPFDenyReason::WrongPhase;
	}
	if (!Placer)
	{
		return EPFDenyReason::InvalidPiece;
	}

	// Never trust the client's Team/Owner — read them from the PlayerState.
	FPFPlacementQuery ServerQ = Q;
	ServerQ.Team = Placer->TeamId;

	const EPFDenyReason Reason = QueryPlacement(ServerQ);
	if (Reason != EPFDenyReason::None)
	{
		UE_LOG(PaintForgeLog, Warning, TEXT("BuildGrid: place %s rejected (%d) for %s"),
			PFPieceTypeName(ServerQ.Type), static_cast<int32>(Reason), *Placer->GetPlayerName());
		return Reason;
	}

	// Rate cap: 10 placements/s/player, checked before the budget spend so a limited request
	// costs nothing (excess is silently dropped client-side per 03 §4).
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	FPFRateWindow& Window = RateWindows.FindOrAdd(Placer->RosterIndex);
	if (Now - Window.WindowStart >= 1.0)
	{
		Window.WindowStart = Now;
		Window.Count = 0;
	}
	if (Window.Count >= 10)
	{
		return EPFDenyReason::RateLimited;
	}

	if (!Placer->ServerTrySpendBudget(ServerQ.Type))
	{
		return EPFDenyReason::OutOfBudget;
	}
	++Window.Count;

	FPFBuildPieceRec Rec;
	Rec.PieceId  = ++NextPieceId;   // monotonic, never reused within a match
	Rec.Type     = ServerQ.Type;
	Rec.X        = ServerQ.X;
	Rec.Y        = ServerQ.Y;
	Rec.Z        = ServerQ.Z;
	Rec.Rot      = ServerQ.Rot;
	Rec.OwnerIdx = Placer->RosterIndex;
	Rec.Team     = ServerQ.Team;

	FPFBuildPieceRec& Added = Pieces.Items.Add_GetRef(Rec);
	Pieces.MarkItemDirty(Added);
	AddPieceLocal(Added);   // server mirror — FastArray callbacks fire on clients only

	OutPieceId = Rec.PieceId;
	UE_LOG(PaintForgeLog, Verbose, TEXT("BuildGrid: %s placed %s #%u at (%d,%d,%d) rot %u"),
		*Placer->GetPlayerName(), PFPieceTypeName(Rec.Type), Rec.PieceId, Rec.X, Rec.Y, Rec.Z, Rec.Rot);
	return EPFDenyReason::None;
}

EPFDenyReason APFBuildGrid::TryDeletePiece(APaintForgePlayerState* Requester, uint16 PieceId)
{
	if (!HasAuthority() || !Requester)
	{
		return EPFDenyReason::NotFound;
	}
	if (bBuildFrozen)
	{
		return EPFDenyReason::WrongPhase;
	}
	const UWorld* World = GetWorld();
	const APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr;
	if (!GS || !GS->IsBuildAllowed())
	{
		return EPFDenyReason::WrongPhase;
	}

	int32 FoundIdx = INDEX_NONE;
	for (int32 Idx = 0; Idx < Pieces.Items.Num(); ++Idx)
	{
		if (Pieces.Items[Idx].PieceId == PieceId)
		{
			FoundIdx = Idx;
			break;
		}
	}
	if (FoundIdx == INDEX_NONE)
	{
		return EPFDenyReason::NotFound;
	}

	const FPFBuildPieceRec Rec = Pieces.Items[FoundIdx];   // copy before removal
	if (Rec.Team != Requester->TeamId)   // teammates may delete team pieces, enemies never (B6)
	{
		return EPFDenyReason::NotYourTeam;
	}

	// 100% refund to the ORIGINAL builder's budget (T19).
	APaintForgePlayerState* Builder = GS->FindPlayerByRosterIndex(Rec.OwnerIdx);
	if (Builder)
	{
		Builder->ServerRefundBudget(Rec.Type);
	}

	RemovePieceLocal(Rec);
	Pieces.Items.RemoveAt(FoundIdx);
	Pieces.MarkArrayDirty();

	// Delete attribution — social pressure is the anti-grief enforcement in v1 (03 §5).
	UE_LOG(PaintForgeLog, Log, TEXT("BuildGrid: %s removed %s's %s #%u"),
		*Requester->GetPlayerName(),
		Builder ? *Builder->GetPlayerName() : TEXT("<gone>"),
		PFPieceTypeName(Rec.Type), Rec.PieceId);
	return EPFDenyReason::None;
}

void APFBuildGrid::FreezeBuild()
{
	bBuildFrozen = true;
	UE_LOG(PaintForgeLog, Log, TEXT("BuildGrid: frozen with %d pieces"), Pieces.Items.Num());
}

void APFBuildGrid::ClearAll()
{
	Pieces.Items.Empty();
	Pieces.MarkArrayDirty();

	for (int32 K = 0; K < 14; ++K)
	{
		if (PieceISMCs[K])
		{
			PieceISMCs[K]->ClearInstances();
		}
		InstanceToPiece[K].Empty();
	}
	PieceToInstance.Empty();
	FloorSlots.Empty();
	InclineSlots.Empty();
	WallEdges.Empty();
	StructuralBounds.Empty();
	PropBounds.Empty();
	RateWindows.Empty();
	NextPieceId = 0;
	bBuildFrozen = false;
	UE_LOG(PaintForgeLog, Log, TEXT("BuildGrid: cleared"));
}

// ---------------------------------------------------------------------------
// Local mirror (ISM instances + occupancy) — server directly, clients via FastArray
// ---------------------------------------------------------------------------

void APFBuildGrid::AddPieceLocal(const FPFBuildPieceRec& Rec)
{
	const int32 K = ISMCIndexFor(Rec.Type, Rec.Team);
	if (PieceISMCs[K])
	{
		const FTransform T = FPFGridMath::PieceLocalTransform(Rec.Type, Rec.X, Rec.Y, Rec.Z, Rec.Rot);
		const int32 InstanceIdx = PieceISMCs[K]->AddInstance(T, /*bWorldSpace=*/true);
		InstanceToPiece[K].Add(InstanceIdx, Rec.PieceId);
		PieceToInstance.Add(Rec.PieceId, InstanceIdx);
	}
	RegisterOccupancy(Rec);
}

void APFBuildGrid::RemovePieceLocal(const FPFBuildPieceRec& Rec)
{
	const int32 K = ISMCIndexFor(Rec.Type, Rec.Team);
	if (PieceISMCs[K])
	{
		if (const int32* InstancePtr = PieceToInstance.Find(Rec.PieceId))
		{
			const int32 InstanceIdx = *InstancePtr;
			const int32 LastIdx = PieceISMCs[K]->GetInstanceCount() - 1;
			PieceISMCs[K]->RemoveInstance(InstanceIdx);   // swap-removal: last instance moves into the hole

			if (InstanceIdx != LastIdx)
			{
				if (const uint16* MovedId = InstanceToPiece[K].Find(LastIdx))
				{
					const uint16 Moved = *MovedId;
					InstanceToPiece[K].Add(InstanceIdx, Moved);
					PieceToInstance.Add(Moved, InstanceIdx);
					InstanceToPiece[K].Remove(LastIdx);
				}
			}
			else
			{
				InstanceToPiece[K].Remove(InstanceIdx);
			}
			PieceToInstance.Remove(Rec.PieceId);
		}
	}
	UnregisterOccupancy(Rec);
}

void APFBuildGrid::RegisterOccupancy(const FPFBuildPieceRec& Rec)
{
	FBox Bounds(ForceInit);
	FPFGridMath::PieceAABB(Rec.Type, Rec.X, Rec.Y, Rec.Z, Rec.Rot, Bounds);

	if (PFIsProp(Rec.Type))
	{
		PropBounds.Add(Rec.PieceId, Bounds);
		return;
	}

	const int32 Cx = Rec.X / PFGrid::SubPerCell;
	const int32 Cy = Rec.Y / PFGrid::SubPerCell;
	const int32 Level = Rec.Z / 3;
	switch (Rec.Type)
	{
	case EPFPieceType::Wall:
		WallEdges.Add(WallEdgeKey(Cx, Cy, Level, Rec.Rot), Rec.PieceId);
		break;
	case EPFPieceType::Floor:
		FloorSlots.Add(FIntVector(Cx, Cy, Level), Rec.PieceId);
		break;
	case EPFPieceType::Ramp:
	case EPFPieceType::Roof:
		InclineSlots.Add(FIntVector(Cx, Cy, Level), Rec.PieceId);
		break;
	default:
		break;
	}
	StructuralBounds.Add(Rec.PieceId, Bounds);
}

void APFBuildGrid::UnregisterOccupancy(const FPFBuildPieceRec& Rec)
{
	if (PFIsProp(Rec.Type))
	{
		PropBounds.Remove(Rec.PieceId);
		return;
	}

	const int32 Cx = Rec.X / PFGrid::SubPerCell;
	const int32 Cy = Rec.Y / PFGrid::SubPerCell;
	const int32 Level = Rec.Z / 3;
	switch (Rec.Type)
	{
	case EPFPieceType::Wall:
		WallEdges.Remove(WallEdgeKey(Cx, Cy, Level, Rec.Rot));
		break;
	case EPFPieceType::Floor:
		FloorSlots.Remove(FIntVector(Cx, Cy, Level));
		break;
	case EPFPieceType::Ramp:
	case EPFPieceType::Roof:
		InclineSlots.Remove(FIntVector(Cx, Cy, Level));
		break;
	default:
		break;
	}
	StructuralBounds.Remove(Rec.PieceId);
}

// ---------------------------------------------------------------------------
// Client lookup
// ---------------------------------------------------------------------------

bool APFBuildGrid::FindPieceByHit(const FHitResult& Hit, uint16& OutPieceId, FPFBuildPieceRec& OutRec) const
{
	const UPrimitiveComponent* HitComp = Hit.GetComponent();
	if (!HitComp || Hit.Item < 0)
	{
		return false;
	}
	for (int32 K = 0; K < 14; ++K)
	{
		if (PieceISMCs[K] != HitComp)
		{
			continue;
		}
		const uint16* IdPtr = InstanceToPiece[K].Find(Hit.Item);
		if (!IdPtr)
		{
			return false;
		}
		for (const FPFBuildPieceRec& Rec : Pieces.Items)
		{
			if (Rec.PieceId == *IdPtr)
			{
				OutPieceId = Rec.PieceId;
				OutRec = Rec;
				return true;
			}
		}
		return false;
	}
	return false;
}
