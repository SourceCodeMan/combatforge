// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFArenaShell.h"

#include "CombatForge.h"
#include "Building/PFBuildPieceVisuals.h"
#include "Core/CombatForgeGameState.h"
#include "Core/PFLightingSubsystem.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	// 1 = soft-load Scene_Warehouse meshes and drape exterior (default). 0 = cube dressing only.
	static TAutoConsoleVariable<int32> CVarWarehouseBackdrop(
		TEXT("pf.WarehouseBackdrop"),
		1,
		TEXT("1 = soft-load warehouse meshes as exterior backdrop drape (NoCollision). 0 = cube dressing only."),
		ECVF_Default);

	UStaticMesh* SoftLoadStaticMesh(const TCHAR* Path)
	{
		if (!Path || !*Path)
		{
			return nullptr;
		}
		return Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad());
	}

	UStaticMesh* SoftLoadFirstMesh(const TCHAR* const* Paths, int32 Count)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			if (UStaticMesh* M = SoftLoadStaticMesh(Paths[i]))
			{
				return M;
			}
		}
		return nullptr;
	}

	template <int32 N>
	UStaticMesh* SoftLoadFirstMesh(const TCHAR* const (&Paths)[N])
	{
		return SoftLoadFirstMesh(Paths, N);
	}
}

namespace
{
	// Map-INVARIANT constants only — FieldX/FieldY (and the Pen*/spacing values derived from them)
	// moved onto the actor as ctor-initialized members (task #40) so APFYardShell can build the
	// same shell at 6400×8000. Everything left here is identical on every map by definition.
	// PerimeterH / EscapeLidZ moved onto the actor too (issue #12 BD1): they must track the MAP's
	// HeightCapUU (Warehouse 1200 → 1800/1350, Yard 2100 → 2700/2250) or a player on high Yard decks
	// jumps the midline barrier / open-field bounds, which were capped at the Warehouse height.
	constexpr float EscapeLidThickness = 40.f;
	constexpr float SpawnZ = 100.f;            // capsule half-height + clearance over the Z=0 floor

	// Warm-up pen: 2000×2000 uu, centered south of the field at Y = -3000 (T29) — every map keeps
	// the pen south (which is why the Yard's facade scenery goes on the NORTH long edge).
	constexpr float PenCenterY = -3000.f;
	constexpr float PenHalf = 1000.f;
	constexpr float PenWallH = 300.f;          // full cover height — unjumpable (03 §1)

	constexpr float PenSlotSpacingX = 160.f;

	// Warehouse dressing — well above HeightCap so build volume stays clean (playbook Z ≥ 1400–1600).
	constexpr float CeilingZ = 1500.f;
	constexpr float TrussZ = 1420.f;
	constexpr float PurlinZ = 1460.f;
}

APFArenaShell::APFArenaShell()
	: APFArenaShell(PFGetArenaMapDef(EPFArenaMap::Warehouse))
{
	// Default = the Warehouse, byte-identical to the pre-selector arena. Map-specific shells are
	// SUBCLASSES delegating the protected ctor below (class identity is the replication channel).
}

APFArenaShell::APFArenaShell(const FPFArenaMapDef& InDef)
	: MapDef(InDef)
	, FieldX(InDef.FieldX)
	, FieldY(InDef.FieldY)
	, PenCenterX(InDef.FieldX * 0.5f)
	, TeamSpawnSpacingY(InDef.FieldY / PFGrid::SpawnPointsPerTeam)
	, PenSlotStartX(InDef.FieldX * 0.5f - PenSlotSpacingX * (PFGrid::MaxRosterSlots - 1) * 0.5f)
	, PerimeterH(static_cast<float>(InDef.HeightCapUU) + 600.f)   // taller than the map's cap + jump
	, EscapeLidZ(static_cast<float>(InDef.HeightCapUU) + 150.f)   // lid just above the map's build cap
{
	PrimaryActorTick.bCanEverTick = false;

	// Replicated for existence only — the geometry is constructor-built identically everywhere.
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(1.f);

	ShellRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ShellRoot"));
	SetRootComponent(ShellRoot);
	ShellRoot->SetMobility(EComponentMobility::Static);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	// Generated triplanar pack (always works on cubes; fallback when warehouse missing).
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FloorFinder(
		TEXT("/Game/Materials/M_PF_ArenaFloor.M_PF_ArenaFloor"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WallFinder(
		TEXT("/Game/Materials/M_PF_ArenaWall.M_PF_ArenaWall"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MetalFinder(
		TEXT("/Game/Materials/M_PF_ArenaMetal.M_PF_ArenaMetal"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MarkFinder(
		TEXT("/Game/Materials/M_PF_ArenaMark.M_PF_ArenaMark"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMatFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	// Scene_Warehouse Megascans surfaces (VT) — preferred when the pack is present.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WhFloorFinder(
		TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/MI_Ind_War_Floor_Concrete_Smooth_01_A.MI_Ind_War_Floor_Concrete_Smooth_01_A"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WhWallFinder(
		TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Wall_Facade_Concrete_New_01/MI_Ind_War_Wall_Facade_Concrete_New_01_A.MI_Ind_War_Wall_Facade_Concrete_New_01_A"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WhMetalFinder(
		TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Sheet_Metal_Rusty_01/MI_Ind_War_Sheet_Metal_Rusty_01_A.MI_Ind_War_Sheet_Metal_Rusty_01_A"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WhTileFinder(
		TEXT("/Game/Scene_Warehouse/VisualFramework/DemoRoom/Materials/M_Tile.M_Tile"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WhDemoMetalFinder(
		TEXT("/Game/Scene_Warehouse/VisualFramework/DemoRoom/Materials/M_Metal.M_Metal"));
	// Heavy Megascans prop meshes are NOT hard-loaded here — CDO TryLoad freezes PIE for minutes.
	// Soft-load in BeginPlay via BuildWarehouseBackdropDrape() instead.
	CubeMesh = CubeFinder.Object;
	PropBarrelMesh = nullptr;
	PropBoxMesh = nullptr;
	PropCeilingLightMesh = nullptr;
	PropLadderMesh = nullptr;
	PropBeamMesh = nullptr;
	PropBulkheadMesh = nullptr;
	PropShelfMesh = nullptr;
	PropPalletMesh = nullptr;
	PropCabinetMesh = nullptr;

	UMaterialInterface* const Basic = BasicMatFinder.Succeeded() ? BasicMatFinder.Object.Get() : nullptr;
	UMaterialInterface* const GenFloor = FloorFinder.Succeeded() ? FloorFinder.Object.Get() : Basic;
	UMaterialInterface* const GenWall = WallFinder.Succeeded() ? WallFinder.Object.Get() : Basic;
	UMaterialInterface* const GenMetal = MetalFinder.Succeeded() ? MetalFinder.Object.Get() : Basic;
	// Prefer warehouse surfaces → demo room tile/metal → generated M_PF_*.
	FloorMaterial = WhFloorFinder.Succeeded() ? WhFloorFinder.Object.Get()
		: (WhTileFinder.Succeeded() ? WhTileFinder.Object.Get() : GenFloor);
	WallMaterial = WhWallFinder.Succeeded() ? WhWallFinder.Object.Get()
		: (WhDemoMetalFinder.Succeeded() ? WhDemoMetalFinder.Object.Get() : GenWall);
	MetalMaterial = WhMetalFinder.Succeeded() ? WhMetalFinder.Object.Get()
		: (WhDemoMetalFinder.Succeeded() ? WhDemoMetalFinder.Object.Get() : GenMetal);
	MarkMaterial = MarkFinder.Succeeded() ? MarkFinder.Object.Get() : Basic;

	// --- Field floor slab: FieldX×FieldY×30, top at Z = 0 (T25: level-0 floors sit flush inside it) ---
	// Cube is 100 uu → scale = size/100. FieldX/100.f is EXACT for both maps (6400/100=64, 8000/100=80),
	// so the Warehouse slab keeps its historical (64, 40, 0.3) scale bit-for-bit.
	FieldFloor = MakeShapePart(TEXT("FieldFloor"),
		FVector(FieldX * 0.5f, FieldY * 0.5f, -15.f), FVector(FieldX / 100.f, FieldY / 100.f, 0.3f),
		EPFShellCollision::SolidBuildable, FloorMaterial);

	// --- 4 perimeter walls (taller than height cap so max-deck + jump can't clear them) ---
	// Cube is 100 uu; scale Z = PerimeterH/100. Slightly long (+0.4) to close corners.
	// Open-field maps (bPerimeter=false, The Yard) build NONE of this: no walls, no escape lid —
	// Tom's spec is a wide-open desert field where you can simply walk off the pad.
	const float WallScaleZ = PerimeterH * 0.01f;
	if (MapDef.bPerimeter)
	{
		PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallN"),
			FVector(FieldX * 0.5f, FieldY + 10.f, PerimeterH * 0.5f), FVector(FieldX / 100.f + 0.4f, 0.2f, WallScaleZ),
			EPFShellCollision::Solid, WallMaterial));
		PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallS"),
			FVector(FieldX * 0.5f, -10.f, PerimeterH * 0.5f), FVector(FieldX / 100.f + 0.4f, 0.2f, WallScaleZ),
			EPFShellCollision::Solid, WallMaterial));
		PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallW"),
			FVector(-10.f, FieldY * 0.5f, PerimeterH * 0.5f), FVector(0.2f, FieldY / 100.f + 0.4f, WallScaleZ),
			EPFShellCollision::Solid, WallMaterial));
		PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallE"),
			FVector(FieldX + 10.f, FieldY * 0.5f, PerimeterH * 0.5f), FVector(0.2f, FieldY / 100.f + 0.4f, WallScaleZ),
			EPFShellCollision::Solid, WallMaterial));

		// Invisible escape lid: blocks pawns from hopping over the build volume rim.
		// Paintballs pass through (Ignore) so high shots still work; build trace ignores it too.
		UStaticMeshComponent* Lid = MakeShapePart(TEXT("EscapeLid"),
			FVector(FieldX * 0.5f, FieldY * 0.5f, EscapeLidZ + EscapeLidThickness * 0.5f),
			FVector(FieldX / 100.f + 0.4f, FieldY / 100.f + 0.4f, EscapeLidThickness * 0.01f),
			EPFShellCollision::Solid, WallMaterial);
		if (Lid)
		{
			Lid->SetVisibility(false);
			Lid->SetHiddenInGame(true);
			Lid->SetCastShadow(false);
			// Only block pawns — not paintballs / build snaps.
			Lid->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Ignore);
			Lid->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Ignore);
			Lid->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
			EscapeLid = Lid;
		}
	}
	else
	{
		// Open-field maps (The Yard): no visible walls, but bound the playable area so a player can't keep
		// walking into the infinite desert and leave the match behind. Invisible, pawn-block ONLY — shots and
		// the build trace pass straight through. Tom 2026-07-17: the old 2000 uu (20 m) margin read as "I can
		// still run outside the bounds", so hold the edge tight — one build cell (400 uu) off the pad.
		const float OpenMargin = 400.f;
		const float BoundZ = PerimeterH * 0.5f;
		const float SpanX = FieldX / 100.f + OpenMargin * 0.02f + 0.4f;   // field width + both margins
		const float SpanY = FieldY / 100.f + OpenMargin * 0.02f + 0.4f;
		auto MakeBound = [&](const TCHAR* Name, const FVector& Loc, const FVector& Scale)
		{
			UStaticMeshComponent* W = MakeShapePart(Name, Loc, Scale, EPFShellCollision::Solid, WallMaterial);
			if (W)
			{
				W->SetVisibility(false);
				W->SetHiddenInGame(true);
				W->SetCastShadow(false);
				W->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Ignore);
				W->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Ignore);
				W->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
				PerimeterWalls.Add(W);
			}
		};
		MakeBound(TEXT("OpenBoundN"), FVector(FieldX * 0.5f, FieldY + OpenMargin, BoundZ), FVector(SpanX, 0.2f, WallScaleZ));
		MakeBound(TEXT("OpenBoundS"), FVector(FieldX * 0.5f, -OpenMargin, BoundZ), FVector(SpanX, 0.2f, WallScaleZ));
		MakeBound(TEXT("OpenBoundW"), FVector(-OpenMargin, FieldY * 0.5f, BoundZ), FVector(0.2f, SpanY, WallScaleZ));
		MakeBound(TEXT("OpenBoundE"), FVector(FieldX + OpenMargin, FieldY * 0.5f, BoundZ), FVector(0.2f, SpanY, WallScaleZ));
	}

	// --- Team-tinted spawn-strip floor tiles over the spawn columns (x = 0 and x = 15) ---
	// Full field width on every map (the Yard's wider strip still reads "my side" at a glance).
	SpawnStrips.Add(MakeShapePart(TEXT("SpawnStripA"),
		FVector(PFGrid::CellUU * 0.5f, FieldY * 0.5f, 1.f), FVector(4.f, FieldY / 100.f, 0.02f),
		EPFShellCollision::Cosmetic, MarkMaterial));
	SpawnStrips.Add(MakeShapePart(TEXT("SpawnStripB"),
		FVector(FieldX - PFGrid::CellUU * 0.5f, FieldY * 0.5f, 1.f), FVector(4.f, FieldY / 100.f, 0.02f),
		EPFShellCollision::Cosmetic, MarkMaterial));

	// --- Midline (T23): 40 uu gray posts every 400 uu + painted floor stripe; occludes nothing ---
	const float MidX = FieldX * 0.5f;   // 3200, center of the neutral strip (cells 7..8)
	// Post count follows the FIELD width (not PFGrid::CellsY) so the see-through fence spans the
	// Yard's open lane too — Warehouse still gets the same 11 posts at the same 400 uu spots.
	const int32 MidlinePostCount = FMath::RoundToInt(FieldY / PFGrid::CellUU);
	for (int32 PostIdx = 0; PostIdx <= MidlinePostCount; ++PostIdx)
	{
		// Z-scale tied to PerimeterH (like the walls) so the base stays on the floor. A hardcoded 12 was the
		// old height-cap value; when the cap rose the posts kept their height but their center tracked
		// PerimeterH, floating them 300 uu off the ground.
		MidlinePosts.Add(MakeShapePart(FString::Printf(TEXT("MidlinePost%d"), PostIdx),
			FVector(MidX, PostIdx * PFGrid::CellUU, PerimeterH * 0.5f), FVector(0.4f, 0.4f, WallScaleZ),
			EPFShellCollision::Solid, MetalMaterial));
	}
	MidlineStripe = MakeShapePart(TEXT("MidlineStripe"),
		FVector(MidX, FieldY * 0.5f, 1.5f), FVector(0.4f, FieldY / 100.f, 0.02f),
		EPFShellCollision::Cosmetic, MarkMaterial);

	// --- The invisible full-height midline blocker (Pawn + Paintball, BuildPhase only) ---
	// On open-field maps there are no perimeter walls to seal the barrier's ends, so it extends
	// 20000 uu into the desert on both sides — otherwise a player could stroll around it and
	// cross into the enemy half during the Build phase.
	const float BarrierHalfY = MapDef.bPerimeter ? FieldY * 0.5f : FieldY * 0.5f + 20000.f;
	MidlineBarrier = CreateDefaultSubobject<UBoxComponent>(TEXT("MidlineBarrier"));
	MidlineBarrier->SetupAttachment(ShellRoot);
	MidlineBarrier->SetRelativeLocation(FVector(MidX, FieldY * 0.5f, PerimeterH * 0.5f));
	MidlineBarrier->InitBoxExtent(FVector(20.f, BarrierHalfY, PerimeterH * 0.5f));
	MidlineBarrier->SetCollisionObjectType(ECC_WorldStatic);
	MidlineBarrier->SetCollisionResponseToAllChannels(ECR_Ignore);
	MidlineBarrier->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	MidlineBarrier->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
	MidlineBarrier->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // off outside BuildPhase

	// --- Warm-up pen: floor + 4 low walls (players cannot jump 300 uu) ---
	PenFloor = MakeShapePart(TEXT("PenFloor"),
		FVector(PenCenterX, PenCenterY, -15.f), FVector(20.f, 20.f, 0.3f),
		EPFShellCollision::Solid, FloorMaterial);
	PenWalls.Add(MakeShapePart(TEXT("PenWallN"),
		FVector(PenCenterX, PenCenterY + PenHalf - 10.f, PenWallH * 0.5f), FVector(20.f, 0.2f, 3.f),
		EPFShellCollision::Solid, WallMaterial));
	PenWalls.Add(MakeShapePart(TEXT("PenWallS"),
		FVector(PenCenterX, PenCenterY - PenHalf + 10.f, PenWallH * 0.5f), FVector(20.f, 0.2f, 3.f),
		EPFShellCollision::Solid, WallMaterial));
	PenWalls.Add(MakeShapePart(TEXT("PenWallW"),
		FVector(PenCenterX - PenHalf + 10.f, PenCenterY, PenWallH * 0.5f), FVector(0.2f, 20.f, 3.f),
		EPFShellCollision::Solid, WallMaterial));
	PenWalls.Add(MakeShapePart(TEXT("PenWallE"),
		FVector(PenCenterX + PenHalf - 10.f, PenCenterY, PenWallH * 0.5f), FVector(0.2f, 20.f, 3.f),
		EPFShellCollision::Solid, WallMaterial));

	// On open-field maps the pen sits 3000 uu south of the field with no perimeter to hide it, so it reads
	// as a stray walled box out in the desert. Keep it functional for warm-up spawns but hide the geometry.
	if (!MapDef.bPerimeter)
	{
		auto HidePart = [](UStaticMeshComponent* P)
		{
			if (P) { P->SetVisibility(false); P->SetHiddenInGame(true); P->SetCastShadow(false); }
		};
		HidePart(PenFloor);
		for (UStaticMeshComponent* W : PenWalls) { HidePart(W); }
	}

	// --- Cosmetic CQB warehouse shell (cube frame; real Megascans drape in BeginPlay) ---
	BuildWarehouseDressing();
}

void APFArenaShell::BuildWarehouseDressing()
{
	// All Cosmetic / NoCollision. Nothing enters the buildable volume (Z < 1200) as solid.
	// Cube mesh is 100 uu; scale = world size / 100.
	// Open-air maps (MapDef.bRoof == false, e.g. The Yard) skip everything that hangs FROM the
	// roof — panels, trusses, purlins, catwalks, bay lights. Wall-mounted / floor-level dressing
	// (ribs, docks, pads, lane marks, bollards) is FieldX/FieldY-relative and kept on every map.

	// Roof deck as a panel GRID with SKYLIGHT openings instead of one solid slab. With Lumen unavailable
	// (no mesh distance fields project-wide), a sealed roof left the interior black — the dynamic sun was
	// fully shadowed out. Leaving ~1/3 of the bays open lets the sun pour straight through onto the floor
	// (real direct light, zero GI cost) and lets the real-time SkyLight capture finally see sky. Openings
	// are diagonally staggered so light reaches every bay including both spawn ends; the trusses/purlins
	// below read as the glazing bars. Tunable: RoofCols/Rows + the skip rule set the open fraction.
	if (MapDef.bRoof)
	{
		constexpr int32 RoofCols = 8;                       // along X: 6400/8 = 800 uu panels
		const int32 RoofRows = FMath::RoundToInt(FieldY / 800.f);   // along Y: 800 uu panels (5 on Warehouse)
		const float PanelW = FieldX / RoofCols;
		const float PanelH = FieldY / RoofRows;
		for (int32 Col = 0; Col < RoofCols; ++Col)
		{
			for (int32 Row = 0; Row < RoofRows; ++Row)
			{
				if (((Col + 2 * Row) % 6) == 1)
				{
					continue;   // skylight opening (7 of 40 ≈ 1/6, staggered) — Tom: 1/3 was "way too much",
					            // cut in half. Ambient for the floor comes from the SkyLight (raised above
					            // the roof to capture sky), NOT roof openness; SkyLight bumped to offset
					            // the lost sun pools.
				}
				DressingParts.Add(MakeShapePart(FString::Printf(TEXT("CeilingPanel%d_%d"), Col, Row),
					FVector((Col + 0.5f) * PanelW, (Row + 0.5f) * PanelH, CeilingZ),
					FVector(PanelW * 0.01f + 0.03f, PanelH * 0.01f + 0.03f, 0.25f),   // +3uu overlap: no seams
					EPFShellCollision::Cosmetic, WallMaterial, FRotator::ZeroRotator, /*bCastShadow=*/true));
			}
		}
	}

	if (MapDef.bRoof)
	{
		// Primary roof trusses — span N–S (along Y) every 800 uu along X. Metal I-beam look.
		const float TrussSpanY = FieldY + 80.f;
		int32 TrussIdx = 0;
		for (float X = 400.f; X < FieldX; X += 800.f)
		{
			const int32 Idx = TrussIdx++;
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("TrussMain%d"), Idx),
				FVector(X, FieldY * 0.5f, TrussZ),
				FVector(0.35f, TrussSpanY * 0.01f, 0.55f),
				EPFShellCollision::Cosmetic, MetalMaterial, FRotator::ZeroRotator, true));
			// Vertical hangers from truss up to ceiling (reads as warehouse structure).
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("TrussHangerN%d"), Idx),
				FVector(X, 200.f, (TrussZ + CeilingZ) * 0.5f),
				FVector(0.15f, 0.15f, (CeilingZ - TrussZ) * 0.01f),
				EPFShellCollision::Cosmetic, MetalMaterial));
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("TrussHangerS%d"), Idx),
				FVector(X, FieldY - 200.f, (TrussZ + CeilingZ) * 0.5f),
				FVector(0.15f, 0.15f, (CeilingZ - TrussZ) * 0.01f),
				EPFShellCollision::Cosmetic, MetalMaterial));
		}

		// Cross purlins — span E–W (along X) every 800 uu along Y.
		const float PurlinSpanX = FieldX + 80.f;
		int32 PurlinIdx = 0;
		for (float Y = 400.f; Y < FieldY; Y += 800.f)
		{
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("Purlin%d"), PurlinIdx++),
				FVector(FieldX * 0.5f, Y, PurlinZ),
				FVector(PurlinSpanX * 0.01f, 0.22f, 0.22f),
				EPFShellCollision::Cosmetic, MetalMaterial, FRotator::ZeroRotator, true));
		}
	}

	// Wall-mounted exterior dressing — ribs, dock doors, corner columns all decorate the arena's
	// own perimeter walls; on open-field maps (no walls) they'd float in the desert, so skip.
	if (MapDef.bPerimeter)
	{
		// Exterior wall ribs / pilasters on the long N/S walls (outside play volume).
		int32 RibIdx = 0;
		for (float X = 200.f; X < FieldX; X += 400.f)
		{
			// North exterior face (Y = FieldY + 30)
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("RibN%d"), RibIdx),
				FVector(X, FieldY + 30.f, PerimeterH * 0.5f),
				FVector(0.35f, 0.25f, 12.2f),
				EPFShellCollision::Cosmetic, WallMaterial));
			// South exterior face
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("RibS%d"), RibIdx),
				FVector(X, -30.f, PerimeterH * 0.5f),
				FVector(0.35f, 0.25f, 12.2f),
				EPFShellCollision::Cosmetic, WallMaterial));
			++RibIdx;
		}

		// Dock-bay doors on spawn walls (west = Team A, east = Team B) — exterior only.
		// Three roll-up bays per side, reading as loading docks over the spawn strips.
		const float DoorH = 7.f;     // 700 uu tall
		const float DoorW = 5.f;     // 500 uu wide
		const float DoorZ = DoorH * 50.f;   // center at half height of door (scale z * 100 / 2)
		const float BayYs[3] = { FieldY * 0.25f, FieldY * 0.5f, FieldY * 0.75f };
		for (int32 i = 0; i < 3; ++i)
		{
			// West exterior (x ≈ -40)
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("DockDoorW%d"), i),
				FVector(-40.f, BayYs[i], DoorZ),
				FVector(0.15f, DoorW, DoorH),
				EPFShellCollision::Cosmetic, MetalMaterial));
			// Header bar above door
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("DockHdrW%d"), i),
				FVector(-40.f, BayYs[i], DoorH * 100.f + 40.f),
				FVector(0.25f, DoorW + 0.4f, 0.35f),
				EPFShellCollision::Cosmetic, MetalMaterial));

			// East exterior (x ≈ FieldX + 40)
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("DockDoorE%d"), i),
				FVector(FieldX + 40.f, BayYs[i], DoorZ),
				FVector(0.15f, DoorW, DoorH),
				EPFShellCollision::Cosmetic, MetalMaterial));
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("DockHdrE%d"), i),
				FVector(FieldX + 40.f, BayYs[i], DoorH * 100.f + 40.f),
				FVector(0.25f, DoorW + 0.4f, 0.35f),
				EPFShellCollision::Cosmetic, MetalMaterial));
		}

		// Corner columns (exterior, visual weight at field corners).
		const FVector Corners[4] = {
			FVector(-50.f, -50.f, PerimeterH * 0.5f),
			FVector(FieldX + 50.f, -50.f, PerimeterH * 0.5f),
			FVector(-50.f, FieldY + 50.f, PerimeterH * 0.5f),
			FVector(FieldX + 50.f, FieldY + 50.f, PerimeterH * 0.5f),
		};
		for (int32 i = 0; i < 4; ++i)
		{
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("CornerCol%d"), i),
				Corners[i], FVector(0.7f, 0.7f, 12.5f),
				EPFShellCollision::Cosmetic, MetalMaterial, FRotator::ZeroRotator, true));
		}
	}

	if (MapDef.bRoof)
	{
		// High catwalk rail hints along long walls just under the trusses (outside play).
		DressingParts.Add(MakeShapePart(TEXT("CatwalkRailN"),
			FVector(FieldX * 0.5f, FieldY + 55.f, 1100.f),
			FVector(FieldX / 100.f, 0.12f, 0.12f),
			EPFShellCollision::Cosmetic, MetalMaterial));
		DressingParts.Add(MakeShapePart(TEXT("CatwalkRailS"),
			FVector(FieldX * 0.5f, -55.f, 1100.f),
			FVector(FieldX / 100.f, 0.12f, 0.12f),
			EPFShellCollision::Cosmetic, MetalMaterial));
	}

	// ---- Playtest polish: more "warehouse arena" silhouette without solid collision ----

	// High-bay light fixtures under the ceiling (emissive-looking metal boxes + glow cores).
	// Roofed maps only — with no ceiling there is nothing for them to hang from.
	if (MapDef.bRoof)
	{
		int32 LightIdx = 0;
		for (float X = 800.f; X < FieldX; X += 1600.f)
		{
			for (float Y = 800.f; Y < FieldY; Y += 1200.f)
			{
				const int32 Idx = LightIdx++;
				// Housing
				DressingParts.Add(MakeShapePart(FString::Printf(TEXT("BayLightH%d"), Idx),
					FVector(X, Y, CeilingZ - 40.f),
					FVector(2.2f, 1.0f, 0.25f),
					EPFShellCollision::Cosmetic, MetalMaterial, FRotator::ZeroRotator, true));
				// Warm "lamp" core (Mark material reads brighter under the lighting rig)
				DressingParts.Add(MakeShapePart(FString::Printf(TEXT("BayLightC%d"), Idx),
					FVector(X, Y, CeilingZ - 55.f),
					FVector(1.8f, 0.7f, 0.08f),
					EPFShellCollision::Cosmetic, MarkMaterial));
			}
		}
	}

	// Interior wall pads / bounce panels just inside the perimeter (cosmetic only, thin).
	// Read as airsoft field padding without blocking movement (NoCollision). Wall-mounted →
	// perimeter maps only.
	if (MapDef.bPerimeter)
	{
		int32 PadIdx = 0;
		for (float X = 400.f; X < FieldX; X += 800.f)
		{
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("PadN%d"), PadIdx),
				FVector(X, FieldY - 25.f, 150.f),
				FVector(3.5f, 0.18f, 3.0f),
				EPFShellCollision::Cosmetic, WallMaterial));
			DressingParts.Add(MakeShapePart(FString::Printf(TEXT("PadS%d"), PadIdx),
				FVector(X, 25.f, 150.f),
				FVector(3.5f, 0.18f, 3.0f),
				EPFShellCollision::Cosmetic, WallMaterial));
			++PadIdx;
		}
	}

	// Floor court rings — dashed lane markers along the long axis (reads as a real field).
	int32 LaneIdx = 0;
	for (float X = 600.f; X < FieldX - 200.f; X += 400.f)
	{
		if (FMath::Abs(X - FieldX * 0.5f) < 250.f)
		{
			continue;   // leave the midline stripe alone
		}
		DressingParts.Add(MakeShapePart(FString::Printf(TEXT("LaneMark%d"), LaneIdx++),
			FVector(X, FieldY * 0.5f, 1.2f),
			FVector(0.6f, 8.f, 0.015f),
			EPFShellCollision::Cosmetic, MarkMaterial));
	}

	// Corner safety bollards (visual weight at spawn corners, outside solid walls).
	const FVector Bollards[4] = {
		FVector(120.f, 120.f, 60.f),
		FVector(FieldX - 120.f, 120.f, 60.f),
		FVector(120.f, FieldY - 120.f, 60.f),
		FVector(FieldX - 120.f, FieldY - 120.f, 60.f),
	};
	for (int32 i = 0; i < 4; ++i)
	{
		DressingParts.Add(MakeShapePart(FString::Printf(TEXT("Bollard%d"), i),
			Bollards[i], FVector(0.45f, 0.45f, 1.2f),
			EPFShellCollision::Cosmetic, MetalMaterial, FRotator::ZeroRotator, true));
	}
}

bool APFArenaShell::SoftLoadWarehouseMeshes()
{
	if (bWarehouseMeshesLoaded)
	{
		return PropBarrelMesh || PropBoxMesh || PropBeamMesh || PropBulkheadMesh
			|| PropShelfMesh || PropCeilingLightMesh || PropLadderMesh;
	}
	bWarehouseMeshesLoaded = true;

	// Paths mirror Content/Scene_Warehouse; first success wins. Safe no-op if pack missing.
	{
		const TCHAR* Paths[] = {
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Aba_Storage_Barrel_Metal_Blue_01/SM_Ind_Aba_Storage_Barrel_Metal_Blue_01.SM_Ind_Aba_Storage_Barrel_Metal_Blue_01"),
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Barrel_Plastic_Blue_01/SM_Ind_War_Storage_Barrel_Plastic_Blue_01.SM_Ind_War_Storage_Barrel_Plastic_Blue_01"),
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Sto_Barrel_Metal_Rust_03/SM_Ind_Sto_Barrel_Metal_Rust_03.SM_Ind_Sto_Barrel_Metal_Rust_03"),
		};
		PropBarrelMesh = SoftLoadFirstMesh(Paths);
	}
	{
		const TCHAR* Paths[] = {
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Box_Cardboard_Set_01/SM_Ind_War_Storage_Box_Cardboard_Set_01_A.SM_Ind_War_Storage_Box_Cardboard_Set_01_A"),
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Box_Cardboard_Set_02/SM_Ind_War_Storage_Box_Cardboard_Set_02_A.SM_Ind_War_Storage_Box_Cardboard_Set_02_A"),
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Box_Cardboard_Worn_02/SM_Ind_War_Storage_Box_Cardboard_Worn_02.SM_Ind_War_Storage_Box_Cardboard_Worn_02"),
		};
		PropBoxMesh = SoftLoadFirstMesh(Paths);
	}
	{
		const TCHAR* Paths[] = {
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Light_Ceiling_Metal_Hanging_01/SM_Ind_War_Light_Ceiling_Metal_Hanging_01.SM_Ind_War_Light_Ceiling_Metal_Hanging_01"),
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Light_Ceiling_Metal_Hanging_02/SM_Ind_War_Light_Ceiling_Metal_Hanging_02.SM_Ind_War_Light_Ceiling_Metal_Hanging_02"),
		};
		PropCeilingLightMesh = SoftLoadFirstMesh(Paths);
	}
	{
		const TCHAR* Paths[] = {
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Equipment_Ladder_Metal_03/SM_Ind_War_Equipment_Ladder_Metal_03.SM_Ind_War_Equipment_Ladder_Metal_03"),
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Equipment_Ladder_Metal_Worn_01/SM_Ind_War_Equipment_Ladder_Metal_Worn_01.SM_Ind_War_Equipment_Ladder_Metal_Worn_01"),
		};
		PropLadderMesh = SoftLoadFirstMesh(Paths);
	}
	{
		const TCHAR* Paths[] = {
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Con_Supplies_Beam_Metal_Rusty_01/SM_Ind_Con_Supplies_Beam_Metal_Rusty_01.SM_Ind_Con_Supplies_Beam_Metal_Rusty_01"),
		};
		PropBeamMesh = SoftLoadFirstMesh(Paths);
	}
	{
		const TCHAR* Paths[] = {
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Bulkhead_Metal_Caged_01/SM_Ind_War_Bulkhead_Metal_Caged_01.SM_Ind_War_Bulkhead_Metal_Caged_01"),
		};
		PropBulkheadMesh = SoftLoadFirstMesh(Paths);
	}
	{
		const TCHAR* Paths[] = {
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Pallet_Shelf_Metal_Modular_01/SM_Ind_War_Pallet_Shelf_Metal_Modular_01_A.SM_Ind_War_Pallet_Shelf_Metal_Modular_01_A"),
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Shelf_Metal_Rusted_02/SM_Ind_War_Storage_Shelf_Metal_Rusted_02.SM_Ind_War_Storage_Shelf_Metal_Rusted_02"),
		};
		PropShelfMesh = SoftLoadFirstMesh(Paths);
	}
	{
		const TCHAR* Paths[] = {
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Pallet_Wood_Worn_01/SM_Ind_War_Storage_Pallet_Wood_Worn_01.SM_Ind_War_Storage_Pallet_Wood_Worn_01"),
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Pallet_Wood_Worn_04/SM_Ind_War_Storage_Pallet_Wood_Worn_04.SM_Ind_War_Storage_Pallet_Wood_Worn_04"),
		};
		PropPalletMesh = SoftLoadFirstMesh(Paths);
	}
	{
		const TCHAR* Paths[] = {
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Cabinet_Metal_Rusty_01/SM_Ind_War_Storage_Cabinet_Metal_Rusty_01.SM_Ind_War_Storage_Cabinet_Metal_Rusty_01"),
			TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Cabinet_Electric_Metal_Dirty_01/SM_Ind_War_Cabinet_Electric_Metal_Dirty_01.SM_Ind_War_Cabinet_Electric_Metal_Dirty_01"),
		};
		PropCabinetMesh = SoftLoadFirstMesh(Paths);
	}

	const int32 Loaded =
		(PropBarrelMesh ? 1 : 0) + (PropBoxMesh ? 1 : 0) + (PropCeilingLightMesh ? 1 : 0)
		+ (PropLadderMesh ? 1 : 0) + (PropBeamMesh ? 1 : 0) + (PropBulkheadMesh ? 1 : 0)
		+ (PropShelfMesh ? 1 : 0) + (PropPalletMesh ? 1 : 0) + (PropCabinetMesh ? 1 : 0);

	UE_LOG(CombatForgeLog, Log,
		TEXT("ArenaShell: warehouse soft-load %d/9 meshes (barrel=%d box=%d light=%d ladder=%d beam=%d bulkhead=%d shelf=%d pallet=%d cabinet=%d)"),
		Loaded,
		PropBarrelMesh ? 1 : 0, PropBoxMesh ? 1 : 0, PropCeilingLightMesh ? 1 : 0,
		PropLadderMesh ? 1 : 0, PropBeamMesh ? 1 : 0, PropBulkheadMesh ? 1 : 0,
		PropShelfMesh ? 1 : 0, PropPalletMesh ? 1 : 0, PropCabinetMesh ? 1 : 0);

	return Loaded > 0;
}

void APFArenaShell::HideCubeDressingByPrefix(const TCHAR* Prefix)
{
	if (!Prefix)
	{
		return;
	}
	for (UStaticMeshComponent* Part : DressingParts)
	{
		if (Part && Part->GetName().StartsWith(Prefix))
		{
			Part->SetVisibility(false);
			Part->SetHiddenInGame(true);
			Part->SetCastShadow(false);
		}
	}
}

UStaticMeshComponent* APFArenaShell::MakeRuntimeMeshPart(const FString& Name, UStaticMesh* Mesh,
                                                         const FVector& Center, const FVector& Scale,
                                                         EPFShellCollision Mode, UMaterialInterface* Material,
                                                         const FRotator& RelRot, bool bCastShadow)
{
	if (!Mesh || !ShellRoot)
	{
		return nullptr;
	}

	UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(this, *Name);
	if (!Comp)
	{
		return nullptr;
	}
	Comp->SetupAttachment(ShellRoot);
	Comp->SetMobility(EComponentMobility::Static);
	Comp->SetStaticMesh(Mesh);
	if (Material)
	{
		Comp->SetMaterial(0, Material);
	}
	// Material == null → keep authored Megascans materials on the mesh.
	Comp->SetRelativeLocation(Center);
	Comp->SetRelativeRotation(RelRot);
	Comp->SetRelativeScale3D(Scale);
	Comp->SetCastShadow(bCastShadow);
	Comp->SetCanEverAffectNavigation(false);
	Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision); // backdrop is always cosmetic
	(void)Mode; // reserved; drape never solid
	Comp->RegisterComponent();
	DressingParts.Add(Comp);
	return Comp;
}

UStaticMeshComponent* APFArenaShell::PlaceBackdropFitted(const FString& Name, UStaticMesh* Mesh,
                                                         const FVector& WorldXY, float GroundZ,
                                                         const FVector& TargetSize, const FRotator& YawRot,
                                                         bool bCastShadow)
{
	if (!Mesh)
	{
		return nullptr;
	}

	const FBoxSphereBounds B = Mesh->GetBounds();
	const FVector MeshSize(
		FMath::Max(B.BoxExtent.X * 2.f, 1.f),
		FMath::Max(B.BoxExtent.Y * 2.f, 1.f),
		FMath::Max(B.BoxExtent.Z * 2.f, 1.f));

	FVector Scale = FVector::OneVector;
	if (TargetSize.X > 1.f && TargetSize.Y > 1.f && TargetSize.Z > 1.f)
	{
		Scale = FVector(
			TargetSize.X / MeshSize.X,
			TargetSize.Y / MeshSize.Y,
			TargetSize.Z / MeshSize.Z);
		// Prefer uniform when extreme squash would look broken.
		const float MaxA = FMath::Max3(Scale.X, Scale.Y, Scale.Z);
		const float MinA = FMath::Min3(Scale.X, Scale.Y, Scale.Z);
		if (MaxA > MinA * 3.5f)
		{
			const float Sc = TargetSize.Z / MeshSize.Z;
			Scale = FVector(Sc, Sc, Sc);
		}
	}

	// Pivot so mesh bottom sits on GroundZ.
	const FVector Loc(
		WorldXY.X - B.Origin.X * Scale.X,
		WorldXY.Y - B.Origin.Y * Scale.Y,
		GroundZ - (B.Origin.Z - B.BoxExtent.Z) * Scale.Z);

	return MakeRuntimeMeshPart(Name, Mesh, Loc, Scale, EPFShellCollision::Cosmetic,
		/*Material=*/nullptr, YawRot, bCastShadow);
}

void APFArenaShell::BuildWarehouseBackdropDrape()
{
	if (bWarehouseDrapeBuilt || bMapBackdropActive)
	{
		return;
	}
	if (CVarWarehouseBackdrop.GetValueOnGameThread() == 0)
	{
		UE_LOG(CombatForgeLog, Log, TEXT("ArenaShell: warehouse backdrop OFF (pf.WarehouseBackdrop=0)"));
		return;
	}
	// Dedicated servers don't render — skip soft-load cost.
	if (const UWorld* World = GetWorld())
	{
		if (World->GetNetMode() == NM_DedicatedServer)
		{
			return;
		}
	}
	if (!SoftLoadWarehouseMeshes())
	{
		UE_LOG(CombatForgeLog, Log,
			TEXT("ArenaShell: no Scene_Warehouse meshes — keeping cube dressing only"));
		return;
	}
	bWarehouseDrapeBuilt = true;

	int32 Spawned = 0;

	// ---- 1) Roof beams under ceiling (replace cube trusses when beam mesh exists) ----
	// Open-air maps have no ceiling: skip the roof-hung drapes (beams, hanging lights) but keep
	// every ground-level exterior prop below — positions are FieldX/FieldY-relative and read as
	// venue clutter stacked against the perimeter on any field size.
	if (MapDef.bRoof && PropBeamMesh)
	{
		const FBoxSphereBounds BB = PropBeamMesh->GetBounds();
		const float MeshLen = FMath::Max(BB.BoxExtent.X, BB.BoxExtent.Y) * 2.f;
		const float SpanY = FieldY + 120.f;
		const float Uniform = (MeshLen > 1.f) ? (SpanY / MeshLen) : 1.f;
		// Orient long axis along Y (N–S). Assume mesh is long on local X → yaw 90.
		int32 Idx = 0;
		for (float X = 400.f; X < FieldX; X += 800.f)
		{
			const FVector Loc(X, FieldY * 0.5f, TrussZ);
			const FVector Scale(Uniform, Uniform * 0.85f, Uniform * 0.85f);
			if (MakeRuntimeMeshPart(FString::Printf(TEXT("DrapeBeam%d"), Idx++),
				PropBeamMesh, Loc, Scale, EPFShellCollision::Cosmetic, nullptr,
				FRotator(0.f, 90.f, 0.f), true))
			{
				++Spawned;
			}
		}
		HideCubeDressingByPrefix(TEXT("TrussMain"));
		HideCubeDressingByPrefix(TEXT("TrussHanger"));
		HideCubeDressingByPrefix(TEXT("Purlin"));
	}

	// ---- 2) Real hanging bay lights (replace cube BayLight*) ----
	if (MapDef.bRoof && PropCeilingLightMesh)
	{
		int32 Idx = 0;
		for (float X = 1000.f; X < FieldX; X += 1600.f)
		{
			for (float Y = 1000.f; Y < FieldY; Y += 1400.f)
			{
				// Hang under ceiling deck; slight scale for visibility.
				if (MakeRuntimeMeshPart(FString::Printf(TEXT("DrapeLight%d"), Idx++),
					PropCeilingLightMesh, FVector(X, Y, CeilingZ - 90.f), FVector(1.15f),
					EPFShellCollision::Cosmetic, nullptr, FRotator::ZeroRotator, true))
				{
					++Spawned;
				}
			}
		}
		HideCubeDressingByPrefix(TEXT("BayLight"));
	}

	// ---- 3) Dock bulkheads on west/east exteriors (replace cube DockDoor*) ----
	if (PropBulkheadMesh)
	{
		const float BayYs[3] = { FieldY * 0.25f, FieldY * 0.5f, FieldY * 0.75f };
		// Target ~500 wide × 80 deep × 700 tall roll-up bay look.
		const FVector BaySize(80.f, 520.f, 720.f);
		for (int32 i = 0; i < 3; ++i)
		{
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapeDockW%d"), i), PropBulkheadMesh,
				FVector(-90.f, BayYs[i], 0.f), 0.f, BaySize, FRotator(0.f, 90.f, 0.f)))
			{
				++Spawned;
			}
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapeDockE%d"), i), PropBulkheadMesh,
				FVector(FieldX + 90.f, BayYs[i], 0.f), 0.f, BaySize, FRotator(0.f, -90.f, 0.f)))
			{
				++Spawned;
			}
		}
		HideCubeDressingByPrefix(TEXT("DockDoor"));
		HideCubeDressingByPrefix(TEXT("DockHdr"));
	}

	// ---- 4) Pallet shelves along exterior long walls (OUTSIDE play Y) ----
	if (PropShelfMesh)
	{
		int32 Idx = 0;
		// ~2.5 m wide racks, ~4 m tall, sit outside Y bounds.
		const FVector ShelfSize(280.f, 120.f, 420.f);
		for (float X = 500.f; X < FieldX; X += 900.f)
		{
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapeShelfS%d"), Idx), PropShelfMesh,
				FVector(X, -160.f, 0.f), 0.f, ShelfSize, FRotator(0.f, 0.f, 0.f)))
			{
				++Spawned;
			}
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapeShelfN%d"), Idx), PropShelfMesh,
				FVector(X, FieldY + 160.f, 0.f), 0.f, ShelfSize, FRotator(0.f, 180.f, 0.f)))
			{
				++Spawned;
			}
			++Idx;
		}
	}

	// ---- 5) Barrels + pallets + box stacks along exteriors ----
	if (PropBarrelMesh)
	{
		int32 Idx = 0;
		const FVector BarrelSize(100.f, 100.f, 140.f);
		for (float X = 700.f; X < FieldX; X += 1100.f)
		{
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapeBarrelS%d"), Idx), PropBarrelMesh,
				FVector(X, -95.f, 0.f), 0.f, BarrelSize, FRotator::ZeroRotator))
			{
				++Spawned;
			}
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapeBarrelN%d"), Idx), PropBarrelMesh,
				FVector(X + 200.f, FieldY + 95.f, 0.f), 0.f, BarrelSize, FRotator(0.f, 40.f, 0.f)))
			{
				++Spawned;
			}
			++Idx;
		}
	}
	if (PropPalletMesh)
	{
		int32 Idx = 0;
		const FVector PalletSize(160.f, 140.f, 25.f);
		for (float X = 900.f; X < FieldX; X += 1400.f)
		{
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapePalletS%d"), Idx), PropPalletMesh,
				FVector(X, -200.f, 0.f), 0.f, PalletSize, FRotator(0.f, 15.f, 0.f)))
			{
				++Spawned;
			}
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapePalletN%d"), Idx), PropPalletMesh,
				FVector(X, FieldY + 200.f, 0.f), 0.f, PalletSize, FRotator(0.f, -20.f, 0.f)))
			{
				++Spawned;
			}
			++Idx;
		}
	}
	if (PropBoxMesh)
	{
		const float BayYs[3] = { FieldY * 0.25f, FieldY * 0.5f, FieldY * 0.75f };
		const FVector BoxSize(180.f, 140.f, 160.f);
		for (int32 i = 0; i < 3; ++i)
		{
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapeBoxW%d"), i), PropBoxMesh,
				FVector(-180.f, BayYs[i] + 220.f, 0.f), 0.f, BoxSize, FRotator::ZeroRotator))
			{
				++Spawned;
			}
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapeBoxE%d"), i), PropBoxMesh,
				FVector(FieldX + 180.f, BayYs[i] - 220.f, 0.f), 0.f, BoxSize, FRotator(0.f, 90.f, 0.f)))
			{
				++Spawned;
			}
		}
	}

	// ---- 6) Ladders mid exterior N/S ----
	if (PropLadderMesh)
	{
		const FVector LadderSize(60.f, 40.f, 500.f);
		if (PlaceBackdropFitted(TEXT("DrapeLadderS"), PropLadderMesh,
			FVector(FieldX * 0.5f, -75.f, 0.f), 0.f, LadderSize, FRotator(0.f, 0.f, 0.f)))
		{
			++Spawned;
		}
		if (PlaceBackdropFitted(TEXT("DrapeLadderN"), PropLadderMesh,
			FVector(FieldX * 0.5f, FieldY + 75.f, 0.f), 0.f, LadderSize, FRotator(0.f, 180.f, 0.f)))
		{
			++Spawned;
		}
	}

	// ---- 7) Cabinets / electrical at dock corners (exterior) ----
	if (PropCabinetMesh)
	{
		const FVector CabSize(90.f, 70.f, 180.f);
		const FVector Cabs[4] = {
			FVector(-130.f, 180.f, 0.f),
			FVector(-130.f, FieldY - 180.f, 0.f),
			FVector(FieldX + 130.f, 180.f, 0.f),
			FVector(FieldX + 130.f, FieldY - 180.f, 0.f),
		};
		const float Yaws[4] = { 90.f, 90.f, -90.f, -90.f };
		for (int32 i = 0; i < 4; ++i)
		{
			if (PlaceBackdropFitted(FString::Printf(TEXT("DrapeCab%d"), i), PropCabinetMesh,
				Cabs[i], 0.f, CabSize, FRotator(0.f, Yaws[i], 0.f)))
			{
				++Spawned;
			}
		}
	}

	// ---- 8) A few props around the warm-up pen (south) for venue continuity ----
	if (PropBarrelMesh)
	{
		const FVector BarrelSize(100.f, 100.f, 140.f);
		if (PlaceBackdropFitted(TEXT("DrapePenBarrel0"), PropBarrelMesh,
			FVector(PenCenterX - 400.f, PenCenterY - PenHalf - 80.f, 0.f), 0.f, BarrelSize,
			FRotator::ZeroRotator))
		{
			++Spawned;
		}
		if (PlaceBackdropFitted(TEXT("DrapePenBarrel1"), PropBarrelMesh,
			FVector(PenCenterX + 450.f, PenCenterY - PenHalf - 90.f, 0.f), 0.f, BarrelSize,
			FRotator(0.f, 55.f, 0.f)))
		{
			++Spawned;
		}
	}
	if (PropPalletMesh)
	{
		if (PlaceBackdropFitted(TEXT("DrapePenPallet"), PropPalletMesh,
			FVector(PenCenterX, PenCenterY - PenHalf - 140.f, 0.f), 0.f,
			FVector(160.f, 140.f, 25.f), FRotator(0.f, 10.f, 0.f)))
		{
			++Spawned;
		}
	}

	// If full map stream later activates, SetMapBackdropActive already hides DressingParts.
	UE_LOG(CombatForgeLog, Log,
		TEXT("ArenaShell: warehouse backdrop drape spawned %d cosmetic meshes (outside/above play volume)"),
		Spawned);
}

UStaticMeshComponent* APFArenaShell::MakeShapePart(const FString& Name, const FVector& Center,
                                                   const FVector& Scale, EPFShellCollision Mode,
                                                   UMaterialInterface* Material, const FRotator& RelRot,
                                                   bool bCastShadow)
{
	return MakeMeshPart(Name, CubeMesh, Center, Scale, Mode, Material, RelRot, bCastShadow);
}

UStaticMeshComponent* APFArenaShell::MakeMeshPart(const FString& Name, UStaticMesh* Mesh, const FVector& Center,
                                                  const FVector& Scale, EPFShellCollision Mode,
                                                  UMaterialInterface* Material, const FRotator& RelRot,
                                                  bool bCastShadow)
{
	if (Mesh == nullptr)
	{
		return nullptr;
	}
	UStaticMeshComponent* Comp = CreateDefaultSubobject<UStaticMeshComponent>(*Name);
	Comp->SetupAttachment(ShellRoot);
	Comp->SetMobility(EComponentMobility::Static);
	Comp->SetStaticMesh(Mesh);
	if (Material)
	{
		Comp->SetMaterial(0, Material);
	}
	Comp->SetRelativeLocation(Center);
	Comp->SetRelativeRotation(RelRot);
	Comp->SetRelativeScale3D(Scale);
	// Solid structure always shadows; cosmetics only when explicitly requested (ceiling/trusses).
	const bool bShadow =
		bCastShadow || Mode == EPFShellCollision::Solid || Mode == EPFShellCollision::SolidBuildable;
	Comp->SetCastShadow(bShadow);

	if (Mode == EPFShellCollision::Cosmetic)
	{
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCanEverAffectNavigation(false);   // backdrop / trusses — purely cosmetic, never navigation
	}
	else
	{
		Comp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		// Solid parts shape the runtime navmesh: the field floor GIVES bots their base walkable surface,
		// and the perimeter walls STOP the mesh at the arena edge so bots never path out of bounds.
		Comp->SetCanEverAffectNavigation(true);
		Comp->SetCollisionObjectType(ECC_WorldStatic);
		Comp->SetCollisionResponseToAllChannels(ECR_Ignore);
		Comp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		Comp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Comp->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		Comp->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Comp->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
		// Only the field floor is a ghost-snap target (§4.6: "pieces + field floor block BuildTrace").
		Comp->SetCollisionResponseToChannel(PF_ECC_BuildTrace,
			(Mode == EPFShellCollision::SolidBuildable) ? ECR_Block : ECR_Ignore);
	}
	return Comp;
}

void APFArenaShell::BeginPlay()
{
	Super::BeginPlay();

	// Cohesion: rebind cube shell surfaces onto the same warehouse triplanar palette as built forts.
	// Must run before ApplyTint so metal posts still get their gray Color MID on the new master.
	ApplyCohesivePalette();

	// Tints (MIDs are runtime objects — never created in the ctor).
	if (SpawnStrips.Num() >= 2)
	{
		// Stronger team-readable spawn paint (kids need to know "my side" at a glance).
		ApplyTint(SpawnStrips[0], PFColors::ForTeam(0));
		ApplyTint(SpawnStrips[1], PFColors::ForTeam(1));
	}
	// Hazard yellow midline stripe; posts get a cold industrial gray metal tint.
	const FLinearColor HazardGray(0.72f, 0.68f, 0.18f);
	const FLinearColor PostGray(0.28f, 0.30f, 0.32f);
	const FLinearColor LaneWhite(0.85f, 0.86f, 0.88f);
	const FLinearColor BayLamp(1.0f, 0.92f, 0.70f);
	ApplyTint(MidlineStripe, HazardGray);
	for (UStaticMeshComponent* Post : MidlinePosts)
	{
		ApplyTint(Post, PostGray);
	}
	// Dressing cosmetics that use MarkMaterial: bay light cores + lane marks.
	for (UStaticMeshComponent* Part : DressingParts)
	{
		if (Part == nullptr)
		{
			continue;
		}
		const FString Name = Part->GetName();
		if (Name.StartsWith(TEXT("BayLightC")))
		{
			ApplyTint(Part, BayLamp);
		}
		else if (Name.StartsWith(TEXT("LaneMark")))
		{
			ApplyTint(Part, LaneWhite);
		}
	}

	// Soft-load Scene_Warehouse meshes and drape exterior props (NoCollision, outside play volume).
	// Cube dressing from the ctor remains as fallback / structure when the pack is missing.
	BuildWarehouseBackdropDrape();

	// Per-map lighting knobs (task #40): the rig is spawned per-machine by UPFLightingSubsystem and
	// the shell exists on every machine too (replicated for existence), so pushing the def here
	// retunes host AND clients with zero extra replication — including a mid-lobby map switch,
	// where the freshly spawned shell class re-runs this on everyone.
	if (UWorld* World = GetWorld())
	{
		if (UPFLightingSubsystem* Lighting = World->GetSubsystem<UPFLightingSubsystem>())
		{
			Lighting->ConfigureForMap(MapDef);
		}
	}

	// Clients mirror the barrier from the replicated phase; the server is driven by the GameMode.
	if (!HasAuthority())
	{
		UWorld* World = GetWorld();
		if (ACombatForgeGameState* GS = World ? World->GetGameState<ACombatForgeGameState>() : nullptr)
		{
			BindToGameState(GS);
		}
		else if (World)
		{
			GameStateSetHandle = World->GameStateSetEvent.AddUObject(this, &APFArenaShell::OnGameStateSet);
		}
	}
}

void APFArenaShell::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (GameStateSetHandle.IsValid())
		{
			World->GameStateSetEvent.Remove(GameStateSetHandle);
			GameStateSetHandle.Reset();
		}
		if (ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>())
		{
			GS->OnPhaseChangedEvent.RemoveAll(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void APFArenaShell::OnGameStateSet(AGameStateBase* NewGameState)
{
	if (ACombatForgeGameState* GS = Cast<ACombatForgeGameState>(NewGameState))
	{
		if (UWorld* World = GetWorld())
		{
			World->GameStateSetEvent.Remove(GameStateSetHandle);
			GameStateSetHandle.Reset();
		}
		BindToGameState(GS);
	}
}

void APFArenaShell::BindToGameState(ACombatForgeGameState* GS)
{
	GS->OnPhaseChangedEvent.RemoveAll(this);
	GS->OnPhaseChangedEvent.AddUObject(this, &APFArenaShell::HandlePhaseChanged);
	HandlePhaseChanged(GS->Phase);   // single-shot tolerant: apply current state immediately (§5.10)
}

void APFArenaShell::HandlePhaseChanged(EPFMatchPhase NewPhase)
{
	SetMidlineBarrierActive(NewPhase == EPFMatchPhase::Build);
}

void APFArenaShell::SetMidlineBarrierActive(bool bActive)
{
	if (!MidlineBarrier)
	{
		return;
	}
	const ECollisionEnabled::Type Wanted =
		bActive ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision;
	if (MidlineBarrier->GetCollisionEnabled() != Wanted)
	{
		MidlineBarrier->SetCollisionEnabled(Wanted);
		UE_LOG(CombatForgeLog, Log, TEXT("ArenaShell: midline barrier %s"),
			bActive ? TEXT("ON") : TEXT("OFF"));
	}
}

void APFArenaShell::SetMapBackdropActive(bool bActive)
{
	if (bMapBackdropActive == bActive)
	{
		return;
	}
	bMapBackdropActive = bActive;

	// Hide cube shell structure + cosmetic dressing so the streamed warehouse map reads as the
	// venue. Keep collision on solid parts; keep spawn strips + midline posts/stripe visible so
	// kids still see team sides and the center line.
	auto SetVis = [bActive](UStaticMeshComponent* Comp)
	{
		if (Comp)
		{
			Comp->SetVisibility(!bActive, /*bPropagateToChildren=*/true);
			Comp->SetHiddenInGame(bActive);
		}
	};

	SetVis(FieldFloor);
	for (UStaticMeshComponent* Wall : PerimeterWalls) { SetVis(Wall); }
	SetVis(PenFloor);
	for (UStaticMeshComponent* Wall : PenWalls) { SetVis(Wall); }
	for (UStaticMeshComponent* Part : DressingParts) { SetVis(Part); }
	// Escape lid stays invisible always (collision-only).
	if (EscapeLid)
	{
		EscapeLid->SetVisibility(false);
		EscapeLid->SetHiddenInGame(true);
	}

	// Spawn strips + midline stay visible (gameplay landmarks).
	UE_LOG(CombatForgeLog, Log, TEXT("ArenaShell: map backdrop %s (cube shell hidden, collision kept)"),
		bActive ? TEXT("ON") : TEXT("OFF"));
}

void APFArenaShell::ApplyCohesivePalette()
{
	// Shared palette with PFBuildGrid. CreatePaletteMID prefers warehouse Surface MIs
	// (working albedo). Do NOT use M_PF_ArenaFloor — pure black base-color graph.
	UMaterialInstanceDynamic* FloorMID = PFBuildPieceVisuals::CreatePaletteMID(
		this, PFBuildPieceVisuals::EPFSurfaceRole::FloorConcrete);
	UMaterialInstanceDynamic* WallMID = PFBuildPieceVisuals::CreatePaletteMID(
		this, PFBuildPieceVisuals::EPFSurfaceRole::WallConcrete);
	UMaterialInstanceDynamic* MetalMID = PFBuildPieceVisuals::CreatePaletteMID(
		this, PFBuildPieceVisuals::EPFSurfaceRole::MetalRusty);
	UMaterialInstanceDynamic* RoofMID = PFBuildPieceVisuals::CreatePaletteMID(
		this, PFBuildPieceVisuals::EPFSurfaceRole::MetalRoof);

	if (FloorMID == nullptr && WallMID == nullptr && MetalMID == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("ArenaShell: cohesion palette missing — keeping CDO materials"));
		return;
	}

	// Remember CDO-assigned materials so we can rewrite every component that still holds them.
	UMaterialInterface* const OldFloor = FloorMaterial.Get();
	UMaterialInterface* const OldWall = WallMaterial.Get();
	UMaterialInterface* const OldMetal = MetalMaterial.Get();

	if (FloorMID) { FloorMaterial = FloorMID; TintMIDs.Add(FloorMID); }
	if (WallMID)  { WallMaterial = WallMID;   TintMIDs.Add(WallMID); }
	if (MetalMID) { MetalMaterial = MetalMID; TintMIDs.Add(MetalMID); }
	if (RoofMID)  { TintMIDs.Add(RoofMID); }

	auto Remap = [&](UStaticMeshComponent* Comp)
	{
		if (Comp == nullptr)
		{
			return;
		}
		UMaterialInterface* Cur = Comp->GetMaterial(0);
		if (Cur == nullptr || (MarkMaterial && Cur == MarkMaterial.Get()))
		{
			return;   // leave Color-driven paint marks alone
		}
		if (OldFloor && Cur == OldFloor && FloorMID)
		{
			Comp->SetMaterial(0, FloorMID);
		}
		else if (OldWall && Cur == OldWall && WallMID)
		{
			// Ceiling decks used WallMaterial at construction — reassign to roof metal for cohesion.
			const FString Name = Comp->GetName();
			if (Name.StartsWith(TEXT("Ceiling")) || Name.StartsWith(TEXT("Purlin")) || Name.Contains(TEXT("Deck")))
			{
				Comp->SetMaterial(0, RoofMID ? RoofMID : WallMID);
			}
			else
			{
				Comp->SetMaterial(0, WallMID);
			}
		}
		else if (OldMetal && Cur == OldMetal && MetalMID)
		{
			Comp->SetMaterial(0, MetalMID);
		}
	};

	Remap(FieldFloor);
	Remap(PenFloor);
	Remap(EscapeLid);
	for (UStaticMeshComponent* W : PerimeterWalls) { Remap(W); }
	for (UStaticMeshComponent* W : PenWalls) { Remap(W); }
	for (UStaticMeshComponent* P : MidlinePosts) { Remap(P); }
	for (UStaticMeshComponent* Part : DressingParts) { Remap(Part); }

	UE_LOG(CombatForgeLog, Log,
		TEXT("ArenaShell: cohesion palette applied (floor=%d wall=%d metal=%d roof=%d)"),
		FloorMID ? 1 : 0, WallMID ? 1 : 0, MetalMID ? 1 : 0, RoofMID ? 1 : 0);
}

void APFArenaShell::ApplyTint(UStaticMeshComponent* Comp, const FLinearColor& Color)
{
	// Spawn strips / hazard lines use MarkMaterial (Color → albedo + soft emissive).
	// Midline posts use MetalMaterial (Color → weak emissive scuff only).
	UMaterialInterface* Parent = MarkMaterial;
	if (Comp)
	{
		// Prefer whatever static material is already on the component (metal vs mark).
		if (UMaterialInterface* Slot0 = Comp->GetMaterial(0))
		{
			Parent = Slot0;
		}
	}
	if (!Comp || !Parent)
	{
		return;
	}
	// Cohesion (ApplyCohesivePalette) may have already placed a shared MID (Floor/Wall/Metal) on this
	// component. A MID cannot parent another MID — UE rejects it ("not a valid parent") and the tint
	// instance renders as the default checker. Walk up to the underlying master material first.
	while (UMaterialInstanceDynamic* ParentMID = Cast<UMaterialInstanceDynamic>(Parent))
	{
		UMaterialInterface* Base = ParentMID->Parent;
		if (Base == nullptr || Base == Parent)
		{
			break;
		}
		Parent = Base;
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, this);
	MID->SetVectorParameterValue(TEXT("Color"), Color);
	Comp->SetMaterial(0, MID);
	TintMIDs.Add(MID);
}

// ---------------------------------------------------------------------------
// Spawn transform providers (deterministic, index-stable)
// ---------------------------------------------------------------------------

FTransform APFArenaShell::GetTeamSpawnTransform(uint8 TeamSide, int32 SlotIdx) const
{
	const int32 Slot = FMath::Clamp(SlotIdx, 0, PFGrid::SpawnPointsPerTeam - 1);
	const bool bWest = (TeamSide != 1);
	const float X = bWest ? PFGrid::CellUU * 0.5f : FieldX - PFGrid::CellUU * 0.5f;   // spawn column centers
	const float Y = (Slot + 0.5f) * TeamSpawnSpacingY;
	const float Yaw = bWest ? 0.f : 180.f;   // face midfield
	return FTransform(FRotator(0.f, Yaw, 0.f), FVector(X, Y, SpawnZ));
}

FTransform APFArenaShell::GetBuildStartTransform(uint8 Team, int32 SlotIdx) const
{
	const int32 Slot = FMath::Clamp(SlotIdx, 0, PFGrid::SpawnPointsPerTeam - 1);
	const bool bTeamA = (Team != 1);
	// On the spawn column (cell 0 / cell 15) — never part of any plot, matching the Combat spawn — so
	// injected community geometry (all-bot half / Improvement map) can never embed a pawn at build start.
	const float X = bTeamA ? 0.5f * PFGrid::CellUU : FieldX - 0.5f * PFGrid::CellUU;
	const float Y = (Slot + 0.5f) * TeamSpawnSpacingY;
	const float Yaw = bTeamA ? 0.f : 180.f;
	return FTransform(FRotator(0.f, Yaw, 0.f), FVector(X, Y, SpawnZ));
}

FTransform APFArenaShell::GetWarmupSpawnTransform(int32 SlotIdx) const
{
	const int32 Slot = FMath::Clamp(SlotIdx, 0, PFGrid::MaxRosterSlots - 1);
	const float X = PenSlotStartX + Slot * PenSlotSpacingX;
	const float Y = PenCenterY - PenHalf + 300.f;   // south line, facing the dummies to the north
	return FTransform(FRotator(0.f, 90.f, 0.f), FVector(X, Y, SpawnZ));
}

FTransform APFArenaShell::GetWarmupDummyTransform(int32 SlotIdx) const
{
	const int32 Slot = FMath::Clamp(SlotIdx, 0, PFGrid::MaxRosterSlots - 1);
	const float X = PenSlotStartX + Slot * PenSlotSpacingX;
	const float Y = PenCenterY + PenHalf - 300.f;   // north line, facing the players
	return FTransform(FRotator(0.f, -90.f, 0.f), FVector(X, Y, 90.f));   // dummy body r≈40 h≈180
}

FTransform APFArenaShell::GetRandomFieldSpawnTransform(int32 Salt) const
{
	// Deterministic-ish per salt so the same restart doesn't always land the same point when
	// callers re-use a stable salt; add time variance via Salt from GameMode.
	FRandomStream Rng(0xA77E0000u ^ static_cast<uint32>(Salt) ^ static_cast<uint32>(Salt * 2654435761u));
	const float Margin = 500.f;   // keep clear of perimeter walls / dock dressing
	const float X = Rng.FRandRange(Margin, FieldX - Margin);
	const float Y = Rng.FRandRange(Margin, FieldY - Margin);
	const float Yaw = Rng.FRandRange(0.f, 360.f);
	return FTransform(FRotator(0.f, Yaw, 0.f), FVector(X, Y, SpawnZ));
}
