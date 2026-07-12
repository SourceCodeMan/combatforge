// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFArenaShell.h"

#include "PaintForge.h"
#include "Core/PaintForgeGameState.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float FieldX = static_cast<float>(PFGrid::CellsX * PFGrid::CellUU);   // 6400
	constexpr float FieldY = static_cast<float>(PFGrid::CellsY * PFGrid::CellUU);   // 4000
	// Perimeter must be taller than HeightCap + jump: players can stand on max-height floors
	// (~1200) and jump over a wall that only reaches the height cap.
	constexpr float PerimeterH = static_cast<float>(PFGrid::HeightCapUU) + 600.f;   // 1800
	// Solid lid just above build cap so you can't leap over the rim from the top deck.
	constexpr float EscapeLidZ = static_cast<float>(PFGrid::HeightCapUU) + 150.f;   // 1350
	constexpr float EscapeLidThickness = 40.f;
	constexpr float SpawnZ = 100.f;            // capsule half-height + clearance over the Z=0 floor

	// Warm-up pen: 2000×2000 uu, centered south of the field at Y = -3000 (T29).
	constexpr float PenCenterX = FieldX * 0.5f;   // 3200
	constexpr float PenCenterY = -3000.f;
	constexpr float PenHalf = 1000.f;
	constexpr float PenWallH = 300.f;          // full cover height — unjumpable (03 §1)

	constexpr float TeamSpawnSpacingY = FieldY / PFGrid::SpawnPointsPerTeam;   // 666.67
	constexpr float PenSlotSpacingX = 160.f;
	constexpr float PenSlotStartX = PenCenterX - PenSlotSpacingX * (PFGrid::MaxRosterSlots - 1) * 0.5f;

	// Warehouse dressing — well above HeightCap so build volume stays clean (playbook Z ≥ 1400–1600).
	constexpr float CeilingZ = 1500.f;
	constexpr float TrussZ = 1420.f;
	constexpr float PurlinZ = 1460.f;
}

APFArenaShell::APFArenaShell()
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
	// Props (optional).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BarrelFinder(
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Aba_Storage_Barrel_Metal_Blue_01/SM_Ind_Aba_Storage_Barrel_Metal_Blue_01.SM_Ind_Aba_Storage_Barrel_Metal_Blue_01"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BoxFinder(
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Box_Cardboard_Set_01/SM_Ind_War_Storage_Box_Cardboard_Set_01_A.SM_Ind_War_Storage_Box_Cardboard_Set_01_A"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> LightFinder(
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Light_Ceiling_Metal_Hanging_01/SM_Ind_War_Light_Ceiling_Metal_Hanging_01.SM_Ind_War_Light_Ceiling_Metal_Hanging_01"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> LadderFinder(
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Equipment_Ladder_Metal_03/SM_Ind_War_Equipment_Ladder_Metal_03.SM_Ind_War_Equipment_Ladder_Metal_03"));

	CubeMesh = CubeFinder.Object;
	PropBarrelMesh = BarrelFinder.Succeeded() ? BarrelFinder.Object.Get() : nullptr;
	PropBoxMesh = BoxFinder.Succeeded() ? BoxFinder.Object.Get() : nullptr;
	PropCeilingLightMesh = LightFinder.Succeeded() ? LightFinder.Object.Get() : nullptr;
	PropLadderMesh = LadderFinder.Succeeded() ? LadderFinder.Object.Get() : nullptr;

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

	// --- Field floor slab: 6400×4000×30, top at Z = 0 (T25: level-0 floors sit flush inside it) ---
	FieldFloor = MakeShapePart(TEXT("FieldFloor"),
		FVector(FieldX * 0.5f, FieldY * 0.5f, -15.f), FVector(64.f, 40.f, 0.3f),
		EPFShellCollision::SolidBuildable, FloorMaterial);

	// --- 4 perimeter walls (taller than height cap so max-deck + jump can't clear them) ---
	// Cube is 100 uu; scale Z = PerimeterH/100. Slightly long to close corners.
	const float WallScaleZ = PerimeterH * 0.01f;
	PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallN"),
		FVector(FieldX * 0.5f, FieldY + 10.f, PerimeterH * 0.5f), FVector(64.4f, 0.2f, WallScaleZ),
		EPFShellCollision::Solid, WallMaterial));
	PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallS"),
		FVector(FieldX * 0.5f, -10.f, PerimeterH * 0.5f), FVector(64.4f, 0.2f, WallScaleZ),
		EPFShellCollision::Solid, WallMaterial));
	PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallW"),
		FVector(-10.f, FieldY * 0.5f, PerimeterH * 0.5f), FVector(0.2f, 40.4f, WallScaleZ),
		EPFShellCollision::Solid, WallMaterial));
	PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallE"),
		FVector(FieldX + 10.f, FieldY * 0.5f, PerimeterH * 0.5f), FVector(0.2f, 40.4f, WallScaleZ),
		EPFShellCollision::Solid, WallMaterial));

	// Invisible escape lid: blocks pawns from hopping over the build volume rim.
	// Paintballs pass through (Ignore) so high shots still work; build trace ignores it too.
	{
		UStaticMeshComponent* Lid = MakeShapePart(TEXT("EscapeLid"),
			FVector(FieldX * 0.5f, FieldY * 0.5f, EscapeLidZ + EscapeLidThickness * 0.5f),
			FVector(64.4f, 40.4f, EscapeLidThickness * 0.01f),
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

	// --- Team-tinted spawn-strip floor tiles over the spawn columns (x = 0 and x = 15) ---
	SpawnStrips.Add(MakeShapePart(TEXT("SpawnStripA"),
		FVector(PFGrid::CellUU * 0.5f, FieldY * 0.5f, 1.f), FVector(4.f, 40.f, 0.02f),
		EPFShellCollision::Cosmetic, MarkMaterial));
	SpawnStrips.Add(MakeShapePart(TEXT("SpawnStripB"),
		FVector(FieldX - PFGrid::CellUU * 0.5f, FieldY * 0.5f, 1.f), FVector(4.f, 40.f, 0.02f),
		EPFShellCollision::Cosmetic, MarkMaterial));

	// --- Midline (T23): 40 uu gray posts every 400 uu + painted floor stripe; occludes nothing ---
	const float MidX = FieldX * 0.5f;   // 3200, center of the neutral strip (cells 7..8)
	for (int32 PostIdx = 0; PostIdx <= PFGrid::CellsY; ++PostIdx)
	{
		MidlinePosts.Add(MakeShapePart(FString::Printf(TEXT("MidlinePost%d"), PostIdx),
			FVector(MidX, PostIdx * PFGrid::CellUU, PerimeterH * 0.5f), FVector(0.4f, 0.4f, 12.f),
			EPFShellCollision::Solid, MetalMaterial));
	}
	MidlineStripe = MakeShapePart(TEXT("MidlineStripe"),
		FVector(MidX, FieldY * 0.5f, 1.5f), FVector(0.4f, 40.f, 0.02f),
		EPFShellCollision::Cosmetic, MarkMaterial);

	// --- The invisible full-height midline blocker (Pawn + Paintball, BuildPhase only) ---
	MidlineBarrier = CreateDefaultSubobject<UBoxComponent>(TEXT("MidlineBarrier"));
	MidlineBarrier->SetupAttachment(ShellRoot);
	MidlineBarrier->SetRelativeLocation(FVector(MidX, FieldY * 0.5f, PerimeterH * 0.5f));
	MidlineBarrier->InitBoxExtent(FVector(20.f, FieldY * 0.5f, PerimeterH * 0.5f));
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

	// --- Cosmetic CQB warehouse shell (no collision, above height cap) ---
	BuildWarehouseDressing();
	BuildWarehouseProps();
}

void APFArenaShell::BuildWarehouseDressing()
{
	// All Cosmetic / NoCollision. Nothing enters the buildable volume (Z < 1200) as solid.
	// Cube mesh is 100 uu; scale = world size / 100.

	// Ceiling deck over the whole field (slightly oversized for clean perimeter join).
	DressingParts.Add(MakeShapePart(TEXT("CeilingDeck"),
		FVector(FieldX * 0.5f, FieldY * 0.5f, CeilingZ),
		FVector(65.f, 41.f, 0.25f),
		EPFShellCollision::Cosmetic, WallMaterial, FRotator::ZeroRotator, /*bCastShadow=*/true));

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

	// High catwalk rail hints along long walls just under the trusses (outside play).
	DressingParts.Add(MakeShapePart(TEXT("CatwalkRailN"),
		FVector(FieldX * 0.5f, FieldY + 55.f, 1100.f),
		FVector(64.f, 0.12f, 0.12f),
		EPFShellCollision::Cosmetic, MetalMaterial));
	DressingParts.Add(MakeShapePart(TEXT("CatwalkRailS"),
		FVector(FieldX * 0.5f, -55.f, 1100.f),
		FVector(64.f, 0.12f, 0.12f),
		EPFShellCollision::Cosmetic, MetalMaterial));

	// ---- Playtest polish: more "warehouse arena" silhouette without solid collision ----

	// High-bay light fixtures under the ceiling (emissive-looking metal boxes + glow cores).
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

	// Interior wall pads / bounce panels just inside the perimeter (cosmetic only, thin).
	// Read as airsoft field padding without blocking movement (NoCollision).
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

void APFArenaShell::BuildWarehouseProps()
{
	// Real Megascans props — all Cosmetic, placed outside the solid play volume so they never
	// steal BuildTrace snaps or block movement. No-ops cleanly if the pack is missing.

	// Barrels along the exterior long walls (outside Y bounds).
	if (PropBarrelMesh)
	{
		int32 Idx = 0;
		for (float X = 600.f; X < FieldX; X += 1000.f)
		{
			DressingParts.Add(MakeMeshPart(FString::Printf(TEXT("PropBarrelS%d"), Idx),
				PropBarrelMesh, FVector(X, -90.f, 0.f), FVector(1.f),
				EPFShellCollision::Cosmetic, nullptr, FRotator::ZeroRotator, true));
			DressingParts.Add(MakeMeshPart(FString::Printf(TEXT("PropBarrelN%d"), Idx),
				PropBarrelMesh, FVector(X, FieldY + 90.f, 0.f), FVector(1.f),
				EPFShellCollision::Cosmetic, nullptr, FRotator(0.f, 35.f, 0.f), true));
			++Idx;
		}
	}

	// Cardboard box stacks outside spawn walls (west / east exteriors).
	if (PropBoxMesh)
	{
		const float BayYs[3] = { FieldY * 0.25f, FieldY * 0.5f, FieldY * 0.75f };
		for (int32 i = 0; i < 3; ++i)
		{
			DressingParts.Add(MakeMeshPart(FString::Printf(TEXT("PropBoxW%d"), i),
				PropBoxMesh, FVector(-120.f, BayYs[i], 0.f), FVector(1.2f),
				EPFShellCollision::Cosmetic, nullptr, FRotator::ZeroRotator, true));
			DressingParts.Add(MakeMeshPart(FString::Printf(TEXT("PropBoxE%d"), i),
				PropBoxMesh, FVector(FieldX + 120.f, BayYs[i], 0.f), FVector(1.2f),
				EPFShellCollision::Cosmetic, nullptr, FRotator(0.f, 90.f, 0.f), true));
		}
	}

	// Real hanging warehouse lights under the ceiling deck.
	if (PropCeilingLightMesh)
	{
		int32 Idx = 0;
		for (float X = 1000.f; X < FieldX; X += 1600.f)
		{
			for (float Y = 1000.f; Y < FieldY; Y += 1400.f)
			{
				DressingParts.Add(MakeMeshPart(FString::Printf(TEXT("PropLight%d"), Idx++),
					PropCeilingLightMesh, FVector(X, Y, CeilingZ - 80.f), FVector(1.f),
					EPFShellCollision::Cosmetic, nullptr, FRotator::ZeroRotator, true));
			}
		}
	}

	// Ladders on exterior mid-wall for silhouette (cosmetic).
	if (PropLadderMesh)
	{
		DressingParts.Add(MakeMeshPart(TEXT("PropLadderS"),
			PropLadderMesh, FVector(FieldX * 0.5f, -70.f, 0.f), FVector(1.f),
			EPFShellCollision::Cosmetic, nullptr, FRotator(0.f, 0.f, 0.f), true));
		DressingParts.Add(MakeMeshPart(TEXT("PropLadderN"),
			PropLadderMesh, FVector(FieldX * 0.5f, FieldY + 70.f, 0.f), FVector(1.f),
			EPFShellCollision::Cosmetic, nullptr, FRotator(0.f, 180.f, 0.f), true));
	}
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
	Comp->SetCanEverAffectNavigation(false);

	if (Mode == EPFShellCollision::Cosmetic)
	{
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	else
	{
		Comp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
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

	// Note: warehouse Megascans props keep authored materials (no tint).

	// Clients mirror the barrier from the replicated phase; the server is driven by the GameMode.
	if (!HasAuthority())
	{
		UWorld* World = GetWorld();
		if (APaintForgeGameState* GS = World ? World->GetGameState<APaintForgeGameState>() : nullptr)
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
		if (APaintForgeGameState* GS = World->GetGameState<APaintForgeGameState>())
		{
			GS->OnPhaseChangedEvent.RemoveAll(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void APFArenaShell::OnGameStateSet(AGameStateBase* NewGameState)
{
	if (APaintForgeGameState* GS = Cast<APaintForgeGameState>(NewGameState))
	{
		if (UWorld* World = GetWorld())
		{
			World->GameStateSetEvent.Remove(GameStateSetHandle);
			GameStateSetHandle.Reset();
		}
		BindToGameState(GS);
	}
}

void APFArenaShell::BindToGameState(APaintForgeGameState* GS)
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
		UE_LOG(PaintForgeLog, Log, TEXT("ArenaShell: midline barrier %s"),
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
	UE_LOG(PaintForgeLog, Log, TEXT("ArenaShell: map backdrop %s (cube shell hidden, collision kept)"),
		bActive ? TEXT("ON") : TEXT("OFF"));
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
