// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFBuildGrid.h"

#include "CombatForge.h"
#include "Building/PFBuildPieceVisuals.h"
#include "Building/PFGridMath.h"
#include "Objectives/PFObjectiveLayout.h"   // connectivity guard: control-point cells
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
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
	// Strict: top of piece must stay under HeightCap so stacked floors can't form a deck you
	// jump over the perimeter from. (Perimeter walls + escape lid also block escape.)
	const int32 Level = Q.Z / 3;
	if (Q.Type == EPFPieceType::Wall && Level > PFGrid::Levels - 2)
	{
		return EPFDenyReason::HeightCap;
	}
	if (!bProp && Level > PFGrid::Levels - 1)
	{
		return EPFDenyReason::HeightCap;
	}
	// Leave a small air gap under the escape lid / wall rim (no piece crowns at 1200 flat).
	constexpr float HeightCapSlackUU = 8.f;
	if (Bounds.Max.Z > static_cast<float>(PFGrid::HeightCapUU) - HeightCapSlackUU)
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

	// --- Connectivity guard (last / most expensive) --- a wall may not seal the map or an objective off.
	if (WouldSealMap(Q))
	{
		return EPFDenyReason::SealsMap;
	}

	return EPFDenyReason::None;
}

bool APFBuildGrid::WouldSealMap(const FPFPlacementQuery& Q) const
{
	// Only a GROUND-LEVEL barrier can sever the cross-map ground path: a WALL, or a RAMP (its HIGH edge
	// stands a full wall-height up — a solid 300 uu face a pawn can't cross on the ground, exactly like a
	// wall). Floors/roofs are walkable, and upper-level pieces don't block the floor. (Spawn columns +
	// neutral strip are no-build, so those cells always stay open — the flood-fill seeds + crosses there.)
	const bool bGroundBarrier = (Q.Type == EPFPieceType::Wall) || (Q.Type == EPFPieceType::Ramp);
	if (!bGroundBarrier || (Q.Z / 3) != 0)
	{
		return false;
	}

	// A ground ramp's high edge sits on its ASCENT side (Rot 0=+X, 1=+Y, 2=-X, 3=-Y). Map it into the same
	// N/E WallEdgeKey space walls use, canonicalizing the -X/-Y sides onto the neighbor cell's E/N edge
	// (a ramp's Rot is a 4-way ascent index, NOT a 2-way wall edge, so it can't go through WallEdgeKey raw).
	auto RampHighEdgeKey = [](int32 Cx, int32 Cy, uint8 Rot) -> FIntVector
	{
		switch (Rot)
		{
		case 0:  return WallEdgeKey(Cx,     Cy,     0, 1);   // +X → E edge of (Cx,Cy)
		case 1:  return WallEdgeKey(Cx,     Cy,     0, 0);   // +Y → N edge of (Cx,Cy)
		case 2:  return WallEdgeKey(Cx - 1, Cy,     0, 1);   // -X → E edge of (Cx-1,Cy)
		default: return WallEdgeKey(Cx,     Cy - 1, 0, 0);   // -Y → N edge of (Cx,Cy-1)
		}
	};

	const int32 QCx = Q.X / PFGrid::SubPerCell;
	const int32 QCy = Q.Y / PFGrid::SubPerCell;
	const FIntVector NewEdge = (Q.Type == EPFPieceType::Ramp)
		? RampHighEdgeKey(QCx, QCy, Q.Rot)
		: WallEdgeKey(QCx, QCy, 0, Q.Rot);

	// Existing ground ramps block their high edge too — collect them once (few pieces on a 16×10 grid).
	TSet<FIntVector> RampEdges;
	for (const FPFBuildPieceRec& Rec : Pieces.Items)
	{
		if (Rec.Type == EPFPieceType::Ramp && (Rec.Z / 3) == 0)
		{
			RampEdges.Add(RampHighEdgeKey(Rec.X / PFGrid::SubPerCell, Rec.Y / PFGrid::SubPerCell, Rec.Rot));
		}
	}

	auto EdgeBlocked = [this, &NewEdge, &RampEdges](int32 Cx, int32 Cy, uint8 EdgeNE) -> bool
	{
		const FIntVector Key = WallEdgeKey(Cx, Cy, 0, EdgeNE);
		return Key == NewEdge || WallEdges.Contains(Key) || RampEdges.Contains(Key);
	};

	constexpr int32 GW = PFGrid::CellsX;   // columns (X)
	constexpr int32 GH = PFGrid::CellsY;   // rows (Y)
	bool Visited[GW * GH] = {};
	int32 Queue[GW * GH];
	int32 Tail = 0;

	// Seed at team A's spawn column, field mid-Y.
	const int32 StartIdx = 0 * GH + (GH / 2);
	Visited[StartIdx] = true;
	Queue[Tail++] = StartIdx;

	auto TryVisit = [&](int32 NX, int32 NY)
	{
		const int32 NI = NX * GH + NY;
		if (!Visited[NI]) { Visited[NI] = true; Queue[Tail++] = NI; }
	};
	for (int32 Head = 0; Head < Tail; ++Head)
	{
		const int32 Cx = Queue[Head] / GH;
		const int32 Cy = Queue[Head] % GH;
		if (Cx + 1 < GW && !EdgeBlocked(Cx, Cy, 1))     { TryVisit(Cx + 1, Cy); }   // E: E-edge of (Cx,Cy)
		if (Cx - 1 >= 0 && !EdgeBlocked(Cx - 1, Cy, 1)) { TryVisit(Cx - 1, Cy); }   // W: E-edge of (Cx-1,Cy)
		if (Cy + 1 < GH && !EdgeBlocked(Cx, Cy, 0))     { TryVisit(Cx, Cy + 1); }   // N: N-edge of (Cx,Cy)
		if (Cy - 1 >= 0 && !EdgeBlocked(Cx, Cy - 1, 0)) { TryVisit(Cx, Cy - 1); }   // S: N-edge of (Cx,Cy-1)
	}

	// Cross-map: team B's spawn column must remain reachable.
	bool bReachB = false;
	for (int32 Y = 0; Y < GH; ++Y) { if (Visited[PFGrid::SpawnColB * GH + Y]) { bReachB = true; break; } }
	if (!bReachB) { return true; }

	// Objectives: every control point must remain reachable (CTF flag homes sit in spawn columns → covered).
	for (int32 i = 0; i < PFObjectiveLayout::ControlPointCount; ++i)
	{
		const FVector Loc = PFObjectiveLayout::ControlPointLocation(i);
		const int32 Ox = FMath::Clamp(FMath::FloorToInt32(Loc.X / static_cast<float>(PFGrid::CellUU)), 0, GW - 1);
		const int32 Oy = FMath::Clamp(FMath::FloorToInt32(Loc.Y / static_cast<float>(PFGrid::CellUU)), 0, GH - 1);
		if (!Visited[Ox * GH + Oy]) { return true; }
	}
	return false;
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
	BuilderByPieceId.Empty();
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
	const int32 K = ISMCIndexFor(Rec.Type, Rec.Team);
	if (PieceISMCs[K])
	{
		const FTransform T = PFBuildPieceVisuals::PieceWorldTransform(Rec.Type, Rec.X, Rec.Y, Rec.Z, Rec.Rot);
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
