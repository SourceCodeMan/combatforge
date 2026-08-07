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
		CubeFinder.Object,       // Roof / ceiling plate (was cone graybox)
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
			// PROPS must be Movable: they swap from these engine placeholders to the real warehouse
			// meshes at runtime (EnsurePieceVisualsApplied), and the engine REFUSES SetStaticMesh on a
			// registered Static component once the world has begun play (the AreDynamicDataChangesAllowed
			// gate). The AUTHORITY dresses the grid during world init — before begin-play — so its swap
			// sticks; a JOINING client receives this actor mid-match, its swap was silently refused
			// ("Calling SetStaticMesh on ... ISM_Barrel_Team0 but Mobility is Static", Tom's 2026-07-23
			// server-join log), and instances stamped with warehouse-FITTED transforms rendered on the
			// unit engine shapes: the "extra large shapes with no skins" every remote joiner saw while
			// the host looked perfect. Structural ISMs never change mesh after the ctor and stay Static.
			ISMC->SetMobility(PFIsProp(static_cast<EPFPieceType>(TypeIdx))
				? EComponentMobility::Movable
				: EComponentMobility::Static);
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

	// Soft-load warehouse prop meshes + structural surface textures after CDO so first compile doesn't freeze PIE.
	PFBuildPieceVisuals::EnsureLoaded();

	// Latch only once the real content is in hand. This used to latch unconditionally, so a client
	// whose first call landed mid-replication (PostReplicatedAdd, below) kept the engine-shape
	// placeholders and checker materials for the rest of the match. Applying is cheap and
	// idempotent, so run it anyway on a partial load — the props look better each pass — but leave
	// the flag clear so the retry timer upgrades them when the content lands.
	bPieceVisualsReady = PFBuildPieceVisuals::IsFullyLoaded();
	bool bMeshSwapped = false;
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
				if (PieceISMCs[K] && PieceISMCs[K]->GetStaticMesh() != PropMesh)
				{
					// SetStaticMesh RETURNS FALSE when the engine refuses it (registered + Static +
					// world begun — the remote-join case; see the ctor mobility comment). That refusal
					// was silent for three alphas: the latch below keyed off CONTENT being loaded, the
					// content loads fine on clients, so the poll stopped while the ISMs still wore the
					// engine placeholders. Never trust the call blindly again — flip mobility and
					// retry once, and if it STILL refuses, log loudly and keep the retry poll alive.
					if (!PieceISMCs[K]->SetStaticMesh(PropMesh))
					{
						PieceISMCs[K]->SetMobility(EComponentMobility::Movable);
						if (!PieceISMCs[K]->SetStaticMesh(PropMesh))
						{
							UE_LOG(CombatForgeLog, Warning,
								TEXT("BuildGrid: prop mesh swap REFUSED on %s (mobility=%d) — keeping retry alive"),
								*PieceISMCs[K]->GetName(),
								static_cast<int32>(PieceISMCs[K]->Mobility));
							bPieceVisualsReady = false;
							continue;
						}
					}
					// Warehouse assets keep their own materials (looks like real cover, not neon cubes).
					if (PFBuildPieceVisuals::UsesNativeMaterials(Type))
					{
						PieceISMCs[K]->EmptyOverrideMaterials();
					}
					// SetStaticMesh / SetMobility on a component ALREADY HOLDING instances tears down
					// and rebuilds its physics state — and UE only pairs a body with an instance added
					// while physics state is fully up, so instances added around this swap desync
					// InstanceBodies from PerInstanceSMData. That desync is the root of the playtest
					// 2026-08-06 invisible walls (RemoveInstance then strands a body that keeps
					// blocking Paintball). Rebuild the bodies from the instance data right here so
					// this component leaves the swap self-consistent.
					if (PieceISMCs[K]->IsPhysicsStateCreated())
					{
						PieceISMCs[K]->RecreatePhysicsState();
					}
					bMeshSwapped = true;
				}
			}
		}
	}

	// Instances placed while a fallback mesh was active carry the fallback's transform — the legacy
	// graybox scales in FPFGridMath::PieceLocalTransform (e.g. PropCan 1.2/1.2/2.2). Those are sized
	// for a UNIT engine cylinder; applied to a real ~1m warehouse barrel they render it several
	// metres tall. Swapping the mesh without re-stamping is how a late-resolving prop turned into a
	// giant. PieceWorldTransform now returns the fitted warehouse transform, so recompute in place.
	if (bMeshSwapped)
	{
		RefreshPropInstanceTransforms();
	}

	// Once the real meshes are in, flip the prop ISMs back to STATIC (playtest 2026-08-06): Movable
	// was only ever needed so SetStaticMesh would be accepted after begin-play (ctor comment). But a
	// Movable ISM makes any pawn standing on a prop use RELATIVE based movement, feeding the same
	// ignored-server-correction desync as the piece-actor parts. Props never move after this point;
	// Static restores absolute corrections (and the mobility change itself rebuilds physics state,
	// re-deriving every per-instance body). Structural ISMs were Static all along.
	if (bPieceVisualsReady)
	{
		for (int32 TypeIdx = 0; TypeIdx < 7; ++TypeIdx)
		{
			const EPFPieceType Type = static_cast<EPFPieceType>(TypeIdx);
			if (!PFIsProp(Type))
			{
				continue;
			}
			for (uint8 Team = 0; Team < 2; ++Team)
			{
				const int32 K = ISMCIndexFor(Type, Team);
				if (PieceISMCs[K] && PieceISMCs[K]->Mobility != EComponentMobility::Static)
				{
					PieceISMCs[K]->SetMobility(EComponentMobility::Static);
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
		// NEVER texture a prop with a structural surface. UsesNativeMaterials is false for a prop only WHILE it's
		// on its engine-shape fallback (mesh not yet loaded) — and this loop would then paint that fallback cone/
		// cylinder/cube with the FloorConcrete MID (RoleForPieceType has no prop case → default FloorConcrete).
		// That's the "cones wearing the ceiling/floor tile" Tom saw. A fallback prop must keep the neutral engine
		// BasicShapeMaterial gray the ctor set (PFBuildGrid ctor SetMaterial), and a loaded prop keeps its native
		// warehouse material (EmptyOverrideMaterials in the swap loop above). So skip props here unconditionally.
		if (PFIsProp(Type) || PFBuildPieceVisuals::UsesNativeMaterials(Type))
		{
			continue;
		}
		// One MID PER TEAM: same warehouse surface either way, with a subtle albedo accent so you can
		// tell your fort from theirs. The old shared-MID version had no team read on structure at
		// all (Tom 2026-07-28 chose the accent over both no-tint and full per-team colour). (P2-BD5)
		for (uint8 Team = 0; Team < 2; ++Team)
		{
			const int32 K = ISMCIndexFor(Type, Team);
			if (!PieceISMCs[K])
			{
				continue;
			}
			UMaterialInstanceDynamic* PaletteMID = PFBuildPieceVisuals::CreateStructuralPaletteMID(this, Type);
			if (PaletteMID != nullptr)
			{
				PFBuildPieceVisuals::ApplyTeamAccent(PaletteMID, Team);
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

	// THE DORITO CONE is the ONE prop that wants a STRUCTURAL skin (the roof/ceiling panel) rather than a
	// native warehouse material — it is always the /Engine Cone, so the prop-skip guard above (which keeps
	// fallback props neutral and warehouse props on their native mats) leaves it on the ctor's gray. The
	// role→material was wired for it (RoleForPieceType(PropDorito)=MetalRoof, and CreateStructuralPaletteMID
	// force-routes the cone through the triplanar+roof profile so the Megascans UVs don't wash it white) —
	// but nothing CALLED it for the cone until now. Skin it explicitly. Safe: the cone never has a native
	// or fallback state to protect (it is the engine Cone by design), so there is no wrong-material race.
	if (UMaterialInstanceDynamic* ConeMID =
		PFBuildPieceVisuals::CreateStructuralPaletteMID(this, EPFPieceType::PropDorito))
	{
		for (uint8 Team = 0; Team < 2; ++Team)
		{
			const int32 K = ISMCIndexFor(EPFPieceType::PropDorito, Team);
			if (PieceISMCs[K])
			{
				PieceISMCs[K]->SetMaterial(0, ConeMID);
				TeamMIDs[K] = ConeMID;
			}
		}
	}

	UE_LOG(CombatForgeLog, Log,
		TEXT("BuildGrid: cohesion palette Wall=%s Floor=%s Ramp=%s Roof=%s (visualsReady=%d)"),
		PFBuildPieceVisuals::StructuralSurfaceName(EPFPieceType::Wall),
		PFBuildPieceVisuals::StructuralSurfaceName(EPFPieceType::Floor),
		PFBuildPieceVisuals::StructuralSurfaceName(EPFPieceType::Ramp),
		PFBuildPieceVisuals::StructuralSurfaceName(EPFPieceType::Roof),
		bPieceVisualsReady ? 1 : 0);

	// Don't wait on the next placed piece to retry — a client that joins a finished fort places
	// nothing, so a partial first load would stay visible all match. Poll until the content lands.
	if (!bPieceVisualsReady)
	{
		StartPieceVisualsRetry();
	}
}

void APFBuildGrid::RefreshPropInstanceTransforms()
{
	int32 Updated = 0;
	for (const FPFBuildPieceRec& Rec : Pieces.Items)
	{
		if (!PFIsProp(Rec.Type))
		{
			continue;
		}
		const int32 K = ISMCIndexFor(Rec.Type, Rec.Team);
		if (K < 0 || K >= 14 || !PieceISMCs[K])
		{
			continue;
		}
		if (const int32* InstancePtr = PieceToInstance.Find(Rec.PieceId))
		{
			const FTransform T = PFBuildPieceVisuals::PieceWorldTransform(Rec.Type, Rec.X, Rec.Y, Rec.Z, Rec.Rot);
			// bMarkRenderStateDirty so the change shows this frame; bTeleport since these are static.
			PieceISMCs[K]->UpdateInstanceTransform(*InstancePtr, T,
				/*bWorldSpace=*/true, /*bMarkRenderStateDirty=*/true, /*bTeleport=*/true);
			++Updated;
		}
	}
	if (Updated > 0)
	{
		UE_LOG(CombatForgeLog, Log, TEXT("BuildGrid: re-stamped %d prop instances after mesh swap"), Updated);
	}
}

void APFBuildGrid::StartPieceVisualsRetry()
{
	// bPieceVisualsGaveUp: the retry budget is per-session, not per-piece. IsTimerActive alone let
	// every AddPieceLocal after give-up start another 30-retry cycle (and re-log "GAVE UP").
	if (bPieceVisualsGaveUp)
	{
		return;
	}
	UWorld* W = GetWorld();
	if (W == nullptr || W->GetTimerManager().IsTimerActive(PieceVisualsRetryHandle))
	{
		return;
	}
	PieceVisualsRetryCount = 0;
	W->GetTimerManager().SetTimer(PieceVisualsRetryHandle, this,
		&APFBuildGrid::TickPieceVisualsRetry, 0.5f, /*bLoop=*/true);
}

void APFBuildGrid::TickPieceVisualsRetry()
{
	// ~15s of half-second retries. Streaming/async-load contention resolves in well under that;
	// past it the content genuinely isn't in the build (a cook gap) and retrying won't help.
	static constexpr int32 MaxRetries = 30;

	++PieceVisualsRetryCount;
	EnsurePieceVisualsApplied();

	if (bPieceVisualsReady || PieceVisualsRetryCount >= MaxRetries)
	{
		if (UWorld* W = GetWorld())
		{
			W->GetTimerManager().ClearTimer(PieceVisualsRetryHandle);
		}
		if (bPieceVisualsReady)
		{
			UE_LOG(CombatForgeLog, Log,
				TEXT("BuildGrid: piece visuals resolved after %d retries"), PieceVisualsRetryCount);
		}
		else
		{
			// Latch BEFORE logging so this warning can only ever print once per session.
			bPieceVisualsGaveUp = true;
			UE_LOG(CombatForgeLog, Warning,
				TEXT("BuildGrid: piece visuals GAVE UP after %d retries — props stay on fallback shapes (check the cook)"),
				PieceVisualsRetryCount);
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

	// --- Height cap / top-story rules (reworked 2026-07-24, Tom: "on the third level I should be
	// able to deploy a wall or window or ceiling tile" — but never a ramp) ---
	// Per piece family on the TOP base (Warehouse level 3 @ Z900, Yard level 6 @ Z1800):
	//   * wall-like: ALLOWED — crowns flush at HeightCap. Safe: the escape lid sits cap+150 and
	//     shell dressing starts ≥ cap+200, so nothing solid is entered ("build volume stays clean").
	//   * ramps: DENIED one story earlier — a ramp must ascend to a base that exists above it;
	//     a top-base ramp is a launch surface toward the lid and leads nowhere.
	//   * floor/roof/trap plates: allowed one level HIGHER than the bases (Level == MapLevels) so a
	//     lid can cap a top-story room flush at HeightCap.
	// The cap check itself is now inclusive (deny only when a piece would EXCEED the cap): the
	// old "-slack" form was the real reason nothing could be built on the third level — every
	// top-story piece crowns exactly at the cap.
	const int32 MapLevels = ActiveLevels();
	const int32 MapHeightCap = ActiveHeightCapUU();
	const int32 Level = Q.Z / 3;
	if (Q.Type == EPFPieceType::Ramp && Level > MapLevels - 2)
	{
		return EPFDenyReason::HeightCap;
	}
	if (PFIsWallLike(Q.Type) && Level > MapLevels - 1)
	{
		return EPFDenyReason::HeightCap;
	}
	if (!bProp && Level > MapLevels)
	{
		return EPFDenyReason::HeightCap;
	}
	constexpr float HeightCapSlackUU = 8.f;
	if (Bounds.Max.Z > static_cast<float>(MapHeightCap) + HeightCapSlackUU)
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

	// Precise deny for the per-type caps (trap floor / one-way door: 1 per player per match) —
	// ServerTrySpendBudget re-checks as a backstop, but from there it's indistinguishable from
	// an empty pool and would read "OUT OF BUDGET" on the HUD.
	if (Placer->ServerIsAtPieceLimit(ServerQ.Type))
	{
		return EPFDenyReason::PieceLimit;
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

	// Same 10/s/player window as placement (issue #12 BD3). The only throttle before was the CLIENT-side
	// self-cap, which a modified client skips — an uncapped delete loop could strip a fort as fast as the
	// RPCs land. Separate window map so deletes don't eat the placement allowance.
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	FPFRateWindow& DelWindow = DeleteRateWindows.FindOrAdd(Requester->RosterIndex);
	if (Now - DelWindow.WindowStart >= 1.0)
	{
		DelWindow.WindowStart = Now;
		DelWindow.Count = 0;
	}
	if (DelWindow.Count >= 10)
	{
		return EPFDenyReason::RateLimited;
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
	++DelWindow.Count;   // count only SUCCESSFUL deletes against the window (mirrors placement)

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
	DeleteRateWindows.Empty();   // else a heavy deleter stays rate-limited into the next Build (P2-BD3)
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

			// INVISIBLE-WALL FIX (playtest 2026-08-06): the ISM's per-instance physics bodies can
			// drift out of sync with PerInstanceSMData — SetStaticMesh/SetMobility in
			// EnsurePieceVisualsApplied recreates physics state on a component already holding live
			// instances, and UE only creates a body per AddInstance while physics state is fully up
			// (InstancedStaticMesh.cpp SetupNewInstanceData). Once desynced, RemoveInstance above
			// terminates the WRONG body (or none) and strands one in the Chaos scene: invisible,
			// unreachable by pawn sweeps, but still answering Paintball traces (that channel's
			// DEFAULT response is Block) — "walk through it, can't shoot through it". Rebuilding the
			// physics state re-derives every body from PerInstanceSMData, evicting any stray. Cost
			// is per-delete on a component with tens of instances, only during Build/Remix.
			if (PieceISMCs[K]->IsPhysicsStateCreated())
			{
				PieceISMCs[K]->RecreatePhysicsState();
			}
			// All 3 playtest "crashes" were the ISM cached-BOUNDS ensure (ISMComponent.h:128): the
			// remove invalidates the bounds cache and something (nav/render) reads it before the
			// lazy recompute. Recompute NOW so the cache is never left invalid — each tripped
			// ensure wrote a synchronous minidump, a multi-second server hitch mid-round.
			PieceISMCs[K]->UpdateBounds();
		}
		else
		{
			// A map miss here means the instance is STILL ALIVE with no owner record — exactly the
			// phantom-blocker precursor. Loud, so a bookkeeping desync shows up in playtest logs.
			UE_LOG(CombatForgeLog, Warning,
				TEXT("BuildGrid: delete of piece %u found no ISM instance mapping (type %d team %d) — possible phantom blocker"),
				Rec.PieceId, static_cast<int32>(Rec.Type), static_cast<int32>(Rec.Team));
		}
	}
	UnregisterOccupancy(Rec);
}

void APFBuildGrid::SpawnRampUnderfill(const FPFBuildPieceRec& Rec)
{
	// REMOVED 2026-08-06 (smoke alpha.17): solid under-ramp steps were invisible walls —
	// crouch approach from behind the high side hit a solid face, and stepped tops also
	// glitched run-offs at the crest. The visual ISM plank alone is the walkable surface;
	// under-ramp crawl is free air. Destroy cleans any leftover holders from older builds.
	DestroyRampUnderfill(Rec.PieceId);
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
	// Spawn AT the grid transform: the actor's root is STATIC now (Static frame parts refuse to
	// attach under a Movable root — the 2026-08-07 "can't place windows" regression), so the old
	// spawn-at-origin + InitFromRecord SetActorLocation dance is no longer possible. Clients place
	// the replicated actor from the spawn bunch; part geometry is world-space either way.
	const FVector GridLoc(Rec.X * PFGrid::SubUU, Rec.Y * PFGrid::SubUU, Rec.Z * PFGrid::SubUU);
	APFBuildPieceActor* Actor = GetWorld()->SpawnActor<APFBuildPieceActor>(
		APFBuildPieceActor::StaticClass(), GridLoc, FRotator::ZeroRotator, Params);
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
	// Non-authority NEVER destroys a replicated actor. The server closes the channel and the client
	// tears the actor down for us; calling Destroy() here races that and produces "attempted to
	// destroy non-authority actor" noise or a brief double-teardown under load. Drop the local
	// bookkeeping and hide it so the piece disappears immediately either way. (P2-BD6)
	if (!HasAuthority())
	{
		if (TObjectPtr<APFBuildPieceActor>* Found = SpecialPieces.Find(PieceId))
		{
			if (IsValid(*Found))
			{
				(*Found)->SetActorHiddenInGame(true);
				(*Found)->SetActorEnableCollision(false);
			}
			SpecialPieces.Remove(PieceId);
		}
		if (GetWorld())
		{
			for (TActorIterator<APFBuildPieceActor> It(GetWorld()); It; ++It)
			{
				if (*It && (*It)->GetPieceId() == PieceId)
				{
					(*It)->SetActorHiddenInGame(true);
					(*It)->SetActorEnableCollision(false);
					break;
				}
			}
		}
		return;
	}

	if (TObjectPtr<APFBuildPieceActor>* Found = SpecialPieces.Find(PieceId))
	{
		if (IsValid(*Found))
		{
			(*Found)->Destroy();
		}
		SpecialPieces.Remove(PieceId);
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
