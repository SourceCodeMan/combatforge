// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFBuildGrid.h"

#include "CombatForge.h"
#include "Building/PFBuildPieceActor.h"
#include "Building/PFBuildPieceVisuals.h"
#include "Building/PFGridMath.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/ConstructorHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/SoftObjectPath.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

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
	// Base/fallback material for pieces + the placement ghost. Placed structural pieces get real warehouse
	// concrete/metal MIs in BeginPlay; this is only the seed + fallback. Prefer the engine BasicShapeMaterial
	// (proven ISM-safe, full-surface "Color" tint) over M_PF_BuildPiece — the latter compiles to the UE CHECKER
	// on instanced static meshes, which is exactly the "checkered walls" bug. Same "Color" call site works.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMatFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ArtMatFinder(TEXT("/Game/Materials/M_PF_BuildPiece.M_PF_BuildPiece"));
	ShapeMaterial = BasicMatFinder.Succeeded() ? BasicMatFinder.Object : ArtMatFinder.Object;

	// Structural + temporary prop placeholders (props swap to warehouse meshes in BeginPlay).
	UStaticMesh* MeshPerType[7] =
	{
		CubeFinder.Object,       // Wall
		CubeFinder.Object,       // Floor
		CubeFinder.Object,       // Ramp (plank)
		ConeFinder.Object,       // Roof (cone proxy)
		CylinderFinder.Object,   // Barrel fallback
		ConeFinder.Object,       // Crate fallback
		CubeFinder.Object        // Boxes fallback
	};

	for (int32 TypeIdx = 0; TypeIdx < 7; ++TypeIdx)
	{
		for (uint8 Team = 0; Team < 2; ++Team)
		{
			const int32 K = ISMCIndexFor(static_cast<EPFPieceType>(TypeIdx), Team);
			const FName CompName(*FString::Printf(TEXT("ISM_%s_Team%d"),
				PFBuildPieceVisuals::DisplayName(static_cast<EPFPieceType>(TypeIdx)), Team));

			UInstancedStaticMeshComponent* ISMC = CreateDefaultSubobject<UInstancedStaticMeshComponent>(CompName);
			ISMC->SetupAttachment(GridRoot);
			ISMC->SetMobility(EComponentMobility::Static);
			ISMC->SetStaticMesh(MeshPerType[TypeIdx]);
			ISMC->SetMaterial(0, ShapeMaterial);
			// Built pieces shape the runtime navmesh so bots PATH AROUND player forts instead of running into
			// them: walls/roofs carve holes, floors/ramps add walkable surface, props (now box-collided) become
			// cover the mesh flows around. Every piece has simple collision (engine cubes/cones + the injected
			// prop boxes), so the ISM's built-in per-instance nav export works and AddInstance auto-dirties the
			// affected tiles (RuntimeGeneration=Dynamic). Was false — that's WHY bots ignored the fort geometry.
			ISMC->SetCanEverAffectNavigation(true);
			ISMC->SetCastShadow(true);

			// RemovePieceLocal's index bookkeeping assumes swap-removal (last instance fills the
			// hole). The engine default is an ordered RemoveAt, which would shift every higher
			// instance index and corrupt InstanceToPiece/PieceToInstance — opt in explicitly.
			ISMC->bSupportRemoveAtSwap = true;

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
	EnsurePieceVisualsApplied();
}

// Prop-mesh swap + cohesion palette. Idempotent, and ALSO called from AddPieceLocal so a joining
// client whose FastArray PostReplicatedAdd fires BEFORE BeginPlay still builds box/barrel/crate
// instances on the real warehouse meshes — not the GCube placeholder, which rendered a client-placed
// box as a GIANT UNSKINNED cube (Adam's 3-player playtest, 2026-07-15).
void APFBuildGrid::EnsurePieceVisualsApplied()
{
	if (bPieceVisualsReady)
	{
		return;
	}
	bPieceVisualsReady = true;

	// Soft-load warehouse prop meshes + structural surface textures after CDO so first compile doesn't freeze PIE.
	PFBuildPieceVisuals::EnsureLoaded();
	for (int32 TypeIdx = 0; TypeIdx < 7; ++TypeIdx)
	{
		const EPFPieceType Type = static_cast<EPFPieceType>(TypeIdx);
		if (!PFIsProp(Type))
		{
			continue;
		}
		if (UStaticMesh* PropMesh = PFBuildPieceVisuals::MeshForType(Type))
		{
			for (uint8 Team = 0; Team < 2; ++Team)
			{
				const int32 K = ISMCIndexFor(Type, Team);
				if (PieceISMCs[K])
				{
					PieceISMCs[K]->SetStaticMesh(PropMesh);
					// Warehouse assets keep their own materials (looks like real cover, not neon cubes).
					if (PFBuildPieceVisuals::UsesNativeMaterials(Type))
					{
						PieceISMCs[K]->EmptyOverrideMaterials();
					}
				}
			}
		}
	}

	// Cohesion palette: triplanar M_PF_Arena* masters + warehouse textures (same stack as arena shell).
	// Avoids mesh-UV Megascans MIs on cubes (smear / near-black walls) and never uses M_PF_BuildPiece
	// (ISM checker). Props keep native warehouse mats.
	for (int32 TypeIdx = 0; TypeIdx < 7; ++TypeIdx)
	{
		const EPFPieceType Type = static_cast<EPFPieceType>(TypeIdx);
		if (PFBuildPieceVisuals::UsesNativeMaterials(Type))
		{
			continue;
		}
		// One MID per type (team tint dropped for texture readability — accepted tradeoff).
		UMaterialInstanceDynamic* PaletteMID = PFBuildPieceVisuals::CreateStructuralPaletteMID(this, Type);
		for (uint8 Team = 0; Team < 2; ++Team)
		{
			const int32 K = ISMCIndexFor(Type, Team);
			if (!PieceISMCs[K])
			{
				continue;
			}
			if (PaletteMID != nullptr)
			{
				PieceISMCs[K]->SetMaterial(0, PaletteMID);
				TeamMIDs[K] = PaletteMID;
			}
			else if (ShapeMaterial != nullptr)
			{
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ShapeMaterial, this);
				MID->SetVectorParameterValue(TEXT("Color"), PFColors::ForTeam(Team));
				PFBuildPieceVisuals::ApplyStructuralSurface(MID, Type);
				PieceISMCs[K]->SetMaterial(0, MID);
				TeamMIDs[K] = MID;
			}
		}
	}

	UE_LOG(CombatForgeLog, Log,
		TEXT("BuildGrid: cohesion palette Wall=%s Floor=%s Ramp=%s Roof=%s"),
		PFBuildPieceVisuals::StructuralSurfaceName(EPFPieceType::Wall),
		PFBuildPieceVisuals::StructuralSurfaceName(EPFPieceType::Floor),
		PFBuildPieceVisuals::StructuralSurfaceName(EPFPieceType::Ramp),
		PFBuildPieceVisuals::StructuralSurfaceName(EPFPieceType::Roof));
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

int32 APFBuildGrid::ActiveCellsY() const
{
	if (const UWorld* W = GetWorld())
	{
		if (const ACombatForgeGameState* GS = W->GetGameState<ACombatForgeGameState>())
		{
			return PFGetArenaMapDef(GS->ArenaMap).CellsY;
		}
	}
	return PFGrid::CellsY;   // pre-GameState fallback = the default (Warehouse) grid
}

int32 APFBuildGrid::ActiveLevels() const
{
	if (const UWorld* W = GetWorld())
	{
		if (const ACombatForgeGameState* GS = W->GetGameState<ACombatForgeGameState>())
		{
			return PFGetArenaMapDef(GS->ArenaMap).Levels;
		}
	}
	return PFGrid::Levels;
}

int32 APFBuildGrid::ActiveHeightCapUU() const
{
	if (const UWorld* W = GetWorld())
	{
		if (const ACombatForgeGameState* GS = W->GetGameState<ACombatForgeGameState>())
		{
			return PFGetArenaMapDef(GS->ArenaMap).HeightCapUU;
		}
	}
	return PFGrid::HeightCapUU;
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
	if (PFIsWallLike(Q.Type) ? (Q.Rot > 1) : (Q.Rot > 3))
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
	const ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
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
	const int32 CY = ActiveCellsY();   // per-map grid rows (Warehouse 10, Yard 20)
	if (PFIsWallLike(Q.Type))
	{
		// A canonical edge belongs to a team if either adjacent cell is in its plot — keeps the
		// front-line edges (plot|neutral) symmetric for both teams.
		const int32 Cx = Q.X / PFGrid::SubPerCell;
		const int32 Cy = Q.Y / PFGrid::SubPerCell;
		const int32 C2x = (Q.Rot == 1) ? Cx + 1 : Cx;
		const int32 C2y = (Q.Rot == 0) ? Cy + 1 : Cy;
		if (!FPFGridMath::IsCellInTeamPlot(Cx, Cy, Q.Team, CY) && !FPFGridMath::IsCellInTeamPlot(C2x, C2y, Q.Team, CY))
		{
			return EPFDenyReason::OutOfPlot;
		}
	}
	else if (!bProp)
	{
		if (!FPFGridMath::IsInsideTeamPlot(Q.Type, Q.X, Q.Y, Q.Team, CY))
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
		const float PlotMaxY = static_cast<float>(CY * PFGrid::CellUU);
		if (Bounds.Min.X < PlotMinX - Eps || Bounds.Max.X > PlotMaxX + Eps ||
			Bounds.Min.Y < -Eps || Bounds.Max.Y > PlotMaxY + Eps)
		{
			return EPFDenyReason::OutOfPlot;
		}
	}

	// --- Height cap (walls: level 0..Levels-2 only — a top-base wall crowns at HeightCap) ---
	// Strict: top of piece must stay under HeightCap so stacked floors can't form a deck you
	// jump over the perimeter from. (Perimeter walls + escape lid also block escape.)
	// Map-specific: Warehouse 4/1200 (3 wall stories under the roof), Yard 7/2100 (6 wall stories).
	const int32 MapLevels = ActiveLevels();
	const int32 MapHeightCap = ActiveHeightCapUU();
	const int32 Level = Q.Z / 3;
	if (PFIsWallLike(Q.Type) && Level > MapLevels - 2)
	{
		return EPFDenyReason::HeightCap;
	}
	if (!bProp && Level > MapLevels - 1)
	{
		return EPFDenyReason::HeightCap;
	}
	// Leave a small air gap under the escape lid / wall rim (no piece crowns at the cap flat).
	constexpr float HeightCapSlackUU = 8.f;
	if (Bounds.Max.Z > static_cast<float>(MapHeightCap) - HeightCapSlackUU)
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
		case EPFPieceType::WallWindow:
		case EPFPieceType::WallDoor:
		case EPFPieceType::WallDoorOneWay:
			if (WallEdges.Contains(WallEdgeKey(Cx, Cy, Level, Q.Rot)))
			{
				return EPFDenyReason::SlotOccupied;
			}
			break;
		case EPFPieceType::Floor:
		case EPFPieceType::FloorTrap:
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

		// Structurals must also not interpenetrate placed props (mirror of the prop branch below,
		// which rejects against StructuralBounds). Shrink so flush contact stays legal.
		const FBox StructShrunk = Bounds.ExpandBy(-1.f);
		for (const TPair<uint16, FBox>& Existing : PropBounds)
		{
			if (StructShrunk.Intersect(Existing.Value))
			{
				return EPFDenyReason::Overlapping;
			}
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

	// No connectivity seal check: forts may fully wall off. Breach with the mid-field bomb.
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

EPFDenyReason APFBuildGrid::TryPlacePiece(ACombatForgePlayerState* Placer, const FPFPlacementQuery& Q, uint16& OutPieceId)
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
		UE_LOG(CombatForgeLog, Warning, TEXT("BuildGrid: place %s rejected (%d) for %s"),
			PFBuildPieceVisuals::DisplayName(ServerQ.Type), static_cast<int32>(Reason), *Placer->GetPlayerName());
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
	BuilderByPieceId.Add(Rec.PieceId, Placer);   // match-stable refund identity (roster indices recycle)

	OutPieceId = Rec.PieceId;
	UE_LOG(CombatForgeLog, Verbose, TEXT("BuildGrid: %s placed %s #%u at (%d,%d,%d) rot %u"),
		*Placer->GetPlayerName(), PFBuildPieceVisuals::DisplayName(Rec.Type), Rec.PieceId, Rec.X, Rec.Y, Rec.Z, Rec.Rot);
	return EPFDenyReason::None;
}

EPFDenyReason APFBuildGrid::TryDeletePiece(ACombatForgePlayerState* Requester, uint16 PieceId)
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
	const ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr;
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

	// 100% refund to the ORIGINAL builder's budget (T19). Roster indices are recycled by the
	// GameMode when a player leaves, so gate the roster lookup on the builder's PlayerState
	// recorded at placement time — otherwise deleting a leaver's pieces would mint budget for
	// whichever newcomer inherited the index.
	ACombatForgePlayerState* Builder = GS->FindPlayerByRosterIndex(Rec.OwnerIdx);
	const TWeakObjectPtr<ACombatForgePlayerState>* StoredBuilder = BuilderByPieceId.Find(Rec.PieceId);
	if (!StoredBuilder || StoredBuilder->Get() != Builder)
	{
		Builder = nullptr;   // index recycled to a different player (or builder gone) — no refund
	}
	if (Builder)
	{
		Builder->ServerRefundBudget(Rec.Type);
	}
	else
	{
		UE_LOG(CombatForgeLog, Log, TEXT("BuildGrid: no refund for piece #%u — original builder (roster %u) left"),
			Rec.PieceId, Rec.OwnerIdx);
	}
	BuilderByPieceId.Remove(Rec.PieceId);

	RemovePieceLocal(Rec);
	Pieces.Items.RemoveAt(FoundIdx);
	Pieces.MarkArrayDirty();

	// Delete attribution — social pressure is the anti-grief enforcement in v1 (03 §5).
	UE_LOG(CombatForgeLog, Log, TEXT("BuildGrid: %s removed %s's %s #%u"),
		*Requester->GetPlayerName(),
		Builder ? *Builder->GetPlayerName() : TEXT("<gone>"),
		PFBuildPieceVisuals::DisplayName(Rec.Type), Rec.PieceId);
	return EPFDenyReason::None;
}

void APFBuildGrid::FreezeBuild()
{
	bBuildFrozen = true;
	UE_LOG(CombatForgeLog, Log, TEXT("BuildGrid: frozen with %d pieces"), Pieces.Items.Num());
}

bool APFBuildGrid::FindPieceById(uint16 PieceId, FPFBuildPieceRec& OutRec) const
{
	for (const FPFBuildPieceRec& Rec : Pieces.Items)
	{
		if (Rec.PieceId == PieceId)
		{
			OutRec = Rec;
			return true;
		}
	}
	return false;
}

void APFBuildGrid::ServerRemovePieceForMatch(uint16 PieceId)
{
	if (!HasAuthority())
	{
		return;
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
		return;   // already gone (deleted/cleared before the fuse ran out)
	}
	// Deliberately NO bBuildFrozen / phase / team / refund gate (unlike TryDeletePiece): a detonation runs
	// during frozen combat and mints no budget. Mutates only the LIVE FastArray — clients mirror the removal
	// via PreReplicatedRemove→RemovePieceLocal, and the saved arena (fingerprinted at Build→Combat) is
	// untouched, so the piece returns next match / on rebuild.
	const FPFBuildPieceRec Rec = Pieces.Items[FoundIdx];   // copy before removal
	BuilderByPieceId.Remove(Rec.PieceId);
	RemovePieceLocal(Rec);
	Pieces.Items.RemoveAt(FoundIdx);
	Pieces.MarkArrayDirty();
	BombedPieceIds.Remove(PieceId);
	UE_LOG(CombatForgeLog, Log, TEXT("BuildGrid: piece #%u (%s) demolished for the match"),
		PieceId, PFBuildPieceVisuals::DisplayName(Rec.Type));
}

bool APFBuildGrid::TrySetPieceBomb(uint16 PieceId)
{
	if (BombedPieceIds.Contains(PieceId))
	{
		return false;
	}
	BombedPieceIds.Add(PieceId);
	return true;
}

void APFBuildGrid::ClearPieceBomb(uint16 PieceId)
{
	BombedPieceIds.Remove(PieceId);
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
	// Destroy special runtime actors (doors / windows / traps).
	for (auto& Pair : SpecialPieces)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	SpecialPieces.Empty();
	for (auto& Pair : RampUnderfills)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	RampUnderfills.Empty();
	if (GetWorld())
	{
		for (TActorIterator<APFBuildPieceActor> It(GetWorld()); It; ++It)
		{
			if (*It)
			{
				(*It)->Destroy();
			}
		}
	}
	FloorSlots.Empty();
	InclineSlots.Empty();
	WallEdges.Empty();
	StructuralBounds.Empty();
	PropBounds.Empty();
	BuilderByPieceId.Empty();
	BombedPieceIds.Empty();
	RateWindows.Empty();
	NextPieceId = 0;
	bBuildFrozen = false;
	UE_LOG(CombatForgeLog, Log, TEXT("BuildGrid: cleared"));
}

void APFBuildGrid::ServerInjectPieces(const TArray<FPFBuildPieceRec>& InPieces)
{
	if (!HasAuthority() || bBuildFrozen)
	{
		return;
	}
	int32 Injected = 0;
	for (const FPFBuildPieceRec& In : InPieces)
	{
		if (In.Team > 1 || In.Type >= EPFPieceType::MAX_Count)
		{
			continue;   // never index the 14-ISM array out of range
		}
		// Same add sequence as TryPlacePiece: re-mint the id, dirty the FastArray, mirror locally.
		// OwnerIdx 255 = community stock (no living builder → delete refunds nothing; team still owns it).
		FPFBuildPieceRec Rec = In;
		Rec.PieceId = ++NextPieceId;   // monotonic; never reuse the source file's id
		Rec.OwnerIdx = 255;
		FPFBuildPieceRec& Added = Pieces.Items.Add_GetRef(Rec);
		Pieces.MarkItemDirty(Added);   // FastArray delta → clients
		AddPieceLocal(Added);          // server-side ISM + occupancy (client path fires via PostReplicatedAdd)
		// Do NOT stamp BuilderByPieceId — teammates may delete, but no budget is minted.
		++Injected;
	}
	if (Injected > 0)
	{
		UE_LOG(CombatForgeLog, Log, TEXT("BuildGrid: injected %d community pieces"), Injected);
	}
}

// ---------------------------------------------------------------------------
// Local mirror (ISM instances + occupancy) — server directly, clients via FastArray
// ---------------------------------------------------------------------------

void APFBuildGrid::AddPieceLocal(const FPFBuildPieceRec& Rec)
{
	EnsurePieceVisualsApplied();   // clients rebuild via PostReplicatedAdd, which can precede BeginPlay
	RegisterOccupancy(Rec);

	// Window / door / trap: server-spawned replicated actor owns collision + state (not an ISM).
	if (PFIsSpecialBuildPiece(Rec.Type))
	{
		if (HasAuthority())
		{
			SpawnSpecialPieceActor(Rec);
		}
		return;
	}

	const int32 K = ISMCIndexFor(Rec.Type, Rec.Team);
	if (K < 0 || K >= 14 || !PieceISMCs[K])
	{
		return;
	}
	const FTransform T = PFBuildPieceVisuals::PieceWorldTransform(Rec.Type, Rec.X, Rec.Y, Rec.Z, Rec.Rot);
	const int32 InstanceIdx = PieceISMCs[K]->AddInstance(T, /*bWorldSpace=*/true);
	InstanceToPiece[K].Add(InstanceIdx, Rec.PieceId);
	PieceToInstance.Add(Rec.PieceId, InstanceIdx);

	// Ramp: fill under the plank so standing heads can't crawl deep under (crouch can).
	if (Rec.Type == EPFPieceType::Ramp)
	{
		SpawnRampUnderfill(Rec);
	}
}

void APFBuildGrid::RemovePieceLocal(const FPFBuildPieceRec& Rec)
{
	if (PFIsSpecialBuildPiece(Rec.Type))
	{
		DestroySpecialPieceActor(Rec.PieceId);
		UnregisterOccupancy(Rec);
		return;
	}

	if (Rec.Type == EPFPieceType::Ramp)
	{
		DestroyRampUnderfill(Rec.PieceId);
	}

	const int32 K = ISMCIndexFor(Rec.Type, Rec.Team);
	if (K >= 0 && K < 14 && PieceISMCs[K])
	{
		if (const int32* InstancePtr = PieceToInstance.Find(Rec.PieceId))
		{
			const int32 InstanceIdx = *InstancePtr;
			const int32 LastIdx = PieceISMCs[K]->GetInstanceCount() - 1;
			PieceISMCs[K]->RemoveInstance(InstanceIdx);   // bSupportRemoveAtSwap (ctor): last instance moves into the hole

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

void APFBuildGrid::SpawnRampUnderfill(const FPFBuildPieceRec& Rec)
{
	UWorld* World = GetWorld();
	if (!World || Rec.Type != EPFPieceType::Ramp)
	{
		return;
	}
	DestroyRampUnderfill(Rec.PieceId);

	// Crouch capsule ≈ 116uu tall (half 58); leave a slightly taller tunnel so crouch isn't sticky.
	// Standing ≈ 176uu — taller than the tunnel, so the solid steps block standing under the ramp.
	// Crouch capsule ≈ 116uu; standing ≈ 176uu. Tunnel is tall enough to crouch-walk under the plank
	// but short enough that a standing head hits the solid steps immediately.
	constexpr float CrouchTunnelUU = 124.f;
	constexpr float CellRunUU = static_cast<float>(PFGrid::CellUU);       // 400
	constexpr float RiseUU = static_cast<float>(PFGrid::WallHeightUU);    // 300
	constexpr int32 Steps = 8;
	constexpr float StepRun = CellRunUU / static_cast<float>(Steps);      // 50
	constexpr float WidthUU = CellRunUU;                                  // full cell width

	const float S = static_cast<float>(PFGrid::SubUU);
	const float Wx = Rec.X * S;
	const float Wy = Rec.Y * S;
	const float Wz = Rec.Z * S;
	const uint8 Rot = Rec.Rot % 4;

	// Low edge mid-point of the cell; ascent Rot 0=+X, 1=+Y, 2=-X, 3=-Y.
	FVector Along = FVector::ZeroVector;
	FVector LowMid = FVector::ZeroVector;
	switch (Rot)
	{
	case 0: // +X — low at min X
		Along = FVector(1.f, 0.f, 0.f);
		LowMid = FVector(Wx, Wy + 200.f, Wz);
		break;
	case 1: // +Y
		Along = FVector(0.f, 1.f, 0.f);
		LowMid = FVector(Wx + 200.f, Wy, Wz);
		break;
	case 2: // -X
		Along = FVector(-1.f, 0.f, 0.f);
		LowMid = FVector(Wx + CellRunUU, Wy + 200.f, Wz);
		break;
	default: // -Y
		Along = FVector(0.f, -1.f, 0.f);
		LowMid = FVector(Wx + 200.f, Wy + CellRunUU, Wz);
		break;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Owner = this;
	Params.ObjectFlags |= RF_Transient;
	AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (!Holder)
	{
		return;
	}
	Holder->SetReplicates(false);
	Holder->SetActorHiddenInGame(true);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Cube)
	{
		Holder->Destroy();
		return;
	}

	for (int32 i = 0; i < Steps; ++i)
	{
		// Distance along ascent from low edge to step midpoint.
		const float DistMid = (static_cast<float>(i) + 0.5f) * StepRun;
		// Underside height of the ramp plank (linear 0→300 over the cell).
		const float PlankZ = (DistMid / CellRunUU) * RiseUU;
		// Solid fill up to just below a crouch tunnel under the plank.
		const float SolidTop = PlankZ - CrouchTunnelUU;
		if (SolidTop < 18.f)
		{
			continue;   // near the low tip — nothing to fill; plank itself is the blocker
		}

		const float SolidHalfH = SolidTop * 0.5f;
		const FVector StepCenter = LowMid
			+ Along * DistMid
			+ FVector(0.f, 0.f, SolidHalfH);

		UStaticMeshComponent* Box = NewObject<UStaticMeshComponent>(Holder,
			*FString::Printf(TEXT("RampFill_%d"), i));
		Box->SetupAttachment(Root);
		Box->SetStaticMesh(Cube);
		Box->SetVisibility(false);
		Box->SetHiddenInGame(true);
		Box->SetCastShadow(false);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Ignore);
		Box->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		Box->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Box->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		Box->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Box->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
		Box->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Block);
		Box->SetCanEverAffectNavigation(true);
		Box->RegisterComponent();
		Box->SetWorldLocation(StepCenter);
		Box->SetWorldRotation(FRotator::ZeroRotator);
		// Axis-aligned: run along X or Y depending on ramp yaw.
		if (Rot == 1 || Rot == 3)
		{
			Box->SetWorldScale3D(FVector(WidthUU / 100.f, StepRun / 100.f, SolidTop / 100.f));
		}
		else
		{
			Box->SetWorldScale3D(FVector(StepRun / 100.f, WidthUU / 100.f, SolidTop / 100.f));
		}
	}

	// Also thicken the *plank* collision slightly via an extra thin plate on the underside so
	// head hits against the slope are reliable (ISM thin cubes can tunnel).
	{
		const FTransform PlankT = FPFGridMath::PieceLocalTransform(
			EPFPieceType::Ramp, Rec.X, Rec.Y, Rec.Z, Rec.Rot);
		UStaticMeshComponent* Plank = NewObject<UStaticMeshComponent>(Holder, TEXT("RampPlankCol"));
		Plank->SetupAttachment(Root);
		Plank->SetStaticMesh(Cube);
		Plank->SetVisibility(false);
		Plank->SetHiddenInGame(true);
		Plank->SetCastShadow(false);
		Plank->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Plank->SetCollisionObjectType(ECC_WorldStatic);
		Plank->SetCollisionResponseToAllChannels(ECR_Ignore);
		Plank->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		Plank->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Plank->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		Plank->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Plank->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
		Plank->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Block);
		Plank->SetCanEverAffectNavigation(true);
		Plank->RegisterComponent();
		// Slightly thicker than the visual 20uu plank so capsule heads catch cleanly.
		FTransform Thick = PlankT;
		const FVector Scale = Thick.GetScale3D();
		Thick.SetScale3D(FVector(Scale.X, Scale.Y, FMath::Max(Scale.Z, 0.55f))); // ~55uu thick
		// Keep the TOP surface roughly where the visual is: shift down along local -Z by half extra thickness.
		const float ExtraHalf = (Thick.GetScale3D().Z - Scale.Z) * 50.f;
		const FVector Down = Thick.GetRotation().RotateVector(FVector(0.f, 0.f, -ExtraHalf));
		Thick.AddToTranslation(Down);
		Plank->SetWorldTransform(Thick);
	}

	RampUnderfills.Add(Rec.PieceId, Holder);
}

void APFBuildGrid::DestroyRampUnderfill(uint16 PieceId)
{
	if (TObjectPtr<AActor>* Found = RampUnderfills.Find(PieceId))
	{
		if (IsValid(*Found))
		{
			(*Found)->Destroy();
		}
		RampUnderfills.Remove(PieceId);
	}
}

void APFBuildGrid::SpawnSpecialPieceActor(const FPFBuildPieceRec& Rec)
{
	if (!HasAuthority() || !GetWorld())
	{
		return;
	}
	DestroySpecialPieceActor(Rec.PieceId);   // belt+braces
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Owner = this;
	APFBuildPieceActor* Actor = GetWorld()->SpawnActor<APFBuildPieceActor>(
		APFBuildPieceActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Actor)
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("BuildGrid: failed to spawn special piece %u type %d"),
			Rec.PieceId, static_cast<int32>(Rec.Type));
		return;
	}
	Actor->InitFromRecord(Rec);
	SpecialPieces.Add(Rec.PieceId, Actor);
}

void APFBuildGrid::DestroySpecialPieceActor(uint16 PieceId)
{
	if (TObjectPtr<APFBuildPieceActor>* Found = SpecialPieces.Find(PieceId))
	{
		if (IsValid(*Found))
		{
			(*Found)->Destroy();
		}
		SpecialPieces.Remove(PieceId);
	}
	// Clients: actor may only exist via replication — also scan by id.
	if (!HasAuthority() && GetWorld())
	{
		for (TActorIterator<APFBuildPieceActor> It(GetWorld()); It; ++It)
		{
			if (*It && (*It)->GetPieceId() == PieceId)
			{
				(*It)->Destroy();
				break;
			}
		}
	}
}

APFBuildPieceActor* APFBuildGrid::FindSpecialPiece(uint16 PieceId) const
{
	if (const TObjectPtr<APFBuildPieceActor>* Found = SpecialPieces.Find(PieceId))
	{
		return Found->Get();
	}
	if (GetWorld())
	{
		for (TActorIterator<APFBuildPieceActor> It(GetWorld()); It; ++It)
		{
			if (*It && (*It)->GetPieceId() == PieceId)
			{
				return *It;
			}
		}
	}
	return nullptr;
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
	case EPFPieceType::WallWindow:
	case EPFPieceType::WallDoor:
	case EPFPieceType::WallDoorOneWay:
		WallEdges.Add(WallEdgeKey(Cx, Cy, Level, Rec.Rot), Rec.PieceId);
		break;
	case EPFPieceType::Floor:
	case EPFPieceType::FloorTrap:
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
	case EPFPieceType::WallWindow:
	case EPFPieceType::WallDoor:
	case EPFPieceType::WallDoorOneWay:
		WallEdges.Remove(WallEdgeKey(Cx, Cy, Level, Rec.Rot));
		break;
	case EPFPieceType::Floor:
	case EPFPieceType::FloorTrap:
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
	if (!HitComp)
	{
		return false;
	}

	// Special piece actors (window / door / trap) — any of their mesh comps.
	if (const APFBuildPieceActor* Special = Cast<APFBuildPieceActor>(Hit.GetActor()))
	{
		const uint16 Id = Special->GetPieceId();
		for (const FPFBuildPieceRec& Rec : Pieces.Items)
		{
			if (Rec.PieceId == Id)
			{
				OutPieceId = Id;
				OutRec = Rec;
				return true;
			}
		}
	}

	if (Hit.Item < 0)
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
