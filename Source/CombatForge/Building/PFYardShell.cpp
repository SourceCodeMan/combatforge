// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFYardShell.h"

#include "CombatForge.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

APFYardShell::APFYardShell()
	: APFArenaShell(PFGetArenaMapDef(EPFArenaMap::Yard))
{
	// Base ctor built the open field (bPerimeter=false: floor pad, spawn strips, midline,
	// desert-length barrier, south pen — no walls, no lid, no roof). We add the venue: the
	// warehouse next door and the desert the whole thing sits in.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SandFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	SandBaseMaterial = SandFinder.Succeeded() ? SandFinder.Object.Get() : nullptr;

	BuildWarehouseFacade();
	BuildDesert();
}

void APFYardShell::BeginPlay()
{
	Super::BeginPlay();

	// Sand MIDs after the world exists. These parts are NOT in DressingParts, so the cohesion
	// palette pass never touches them — without this tint they'd render BasicShape grey.
	if (SandBaseMaterial != nullptr)
	{
		for (int32 i = 0; i < SandParts.Num(); ++i)
		{
			UStaticMeshComponent* Part = SandParts[i];
			if (Part == nullptr)
			{
				continue;
			}
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(SandBaseMaterial, this);
			MID->SetVectorParameterValue(TEXT("Color"),
				SandTints.IsValidIndex(i) ? SandTints[i] : FLinearColor(0.72f, 0.62f, 0.45f));
			Part->SetMaterial(0, MID);
		}
	}
	else
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("YardShell: BasicShapeMaterial missing — desert renders untinted"));
	}
}

void APFYardShell::BuildWarehouseFacade()
{
	// The warehouse the Warehouse map plays INSIDE, seen from outside: a windowless cube-primitive
	// building filling Y ∈ [FieldY+200, FieldY+4200] across the full field length. NORTH edge
	// because the warm-up pen owns the south exterior (Y=-3000, base ctor). With no perimeter
	// wall anymore, players can walk right up to the building — so its four WALLS are Solid
	// (you bump into the warehouse, you don't ghost through it); roof/ribs/docks stay Cosmetic
	// (unreachable). All registered in DressingParts so the cohesion palette pass
	// (ApplyCohesivePalette) rebinds them like every other dressing cube.
	const float FacadeS = FieldY + 200.f;              // yard-facing (south) face
	const float FacadeN = FieldY + 4200.f;             // back face — 4000 uu deep, like the arena
	const float FacadeMidY = (FacadeS + FacadeN) * 0.5f;
	const float FacadeWallH = 1800.f;                  // same parapet height as the arena perimeter
	const float FacadeRoofZ = 1500.f;                  // same deck height as the warehouse ceiling
	const float FacadeDepthScale = (FacadeN - FacadeS) / 100.f;   // cube is 100 uu → 40

	// 4 facade walls (thin slabs; +0.4 length closes the corners, mirroring the arena perimeter).
	// bCastShadow on the south wall + roof is the payoff: the building shades the near lane.
	DressingParts.Add(MakeShapePart(TEXT("FacadeWallS"),
		FVector(FieldX * 0.5f, FacadeS, FacadeWallH * 0.5f),
		FVector(FieldX / 100.f + 0.4f, 0.3f, FacadeWallH / 100.f),
		EPFShellCollision::Solid, WallMaterial, FRotator::ZeroRotator, /*bCastShadow=*/true));
	DressingParts.Add(MakeShapePart(TEXT("FacadeWallN"),
		FVector(FieldX * 0.5f, FacadeN, FacadeWallH * 0.5f),
		FVector(FieldX / 100.f + 0.4f, 0.3f, FacadeWallH / 100.f),
		EPFShellCollision::Solid, WallMaterial, FRotator::ZeroRotator, true));
	DressingParts.Add(MakeShapePart(TEXT("FacadeWallW"),
		FVector(0.f, FacadeMidY, FacadeWallH * 0.5f),
		FVector(0.3f, FacadeDepthScale + 0.4f, FacadeWallH / 100.f),
		EPFShellCollision::Solid, WallMaterial, FRotator::ZeroRotator, true));
	DressingParts.Add(MakeShapePart(TEXT("FacadeWallE"),
		FVector(FieldX, FacadeMidY, FacadeWallH * 0.5f),
		FVector(0.3f, FacadeDepthScale + 0.4f, FacadeWallH / 100.f),
		EPFShellCollision::Solid, WallMaterial, FRotator::ZeroRotator, true));

	// Solid roof deck ("Deck" in the name → cohesion pass rebinds it to the roof-metal MID).
	DressingParts.Add(MakeShapePart(TEXT("FacadeRoofDeck"),
		FVector(FieldX * 0.5f, FacadeMidY, FacadeRoofZ),
		FVector(FieldX / 100.f + 0.4f, FacadeDepthScale + 0.4f, 0.3f),
		EPFShellCollision::Cosmetic, WallMaterial, FRotator::ZeroRotator, true));

	// Pilaster ribs on the yard-facing face — same rhythm/size as the arena's exterior ribs.
	int32 RibIdx = 0;
	for (float X = 200.f; X < FieldX; X += 400.f)
	{
		DressingParts.Add(MakeShapePart(FString::Printf(TEXT("FacadeRib%d"), RibIdx++),
			FVector(X, FacadeS - 30.f, FacadeWallH * 0.5f),
			FVector(0.35f, 0.25f, 12.2f),
			EPFShellCollision::Cosmetic, WallMaterial));
	}

	// Three roll-up dock doors + header bars on the yard-facing face (loading docks toward the
	// field — the arena's own docks use the same 500×700 read on its spawn walls).
	const float DoorH = 7.f;                 // 700 uu tall
	const float DoorW = 5.f;                 // 500 uu wide
	const float DoorZ = DoorH * 50.f;        // center at half height of door
	const float BayXs[3] = { FieldX * 0.25f, FieldX * 0.5f, FieldX * 0.75f };
	for (int32 i = 0; i < 3; ++i)
	{
		DressingParts.Add(MakeShapePart(FString::Printf(TEXT("FacadeDockDoor%d"), i),
			FVector(BayXs[i], FacadeS - 40.f, DoorZ),
			FVector(DoorW, 0.15f, DoorH),
			EPFShellCollision::Cosmetic, MetalMaterial));
		DressingParts.Add(MakeShapePart(FString::Printf(TEXT("FacadeDockHdr%d"), i),
			FVector(BayXs[i], FacadeS - 40.f, DoorH * 100.f + 40.f),
			FVector(DoorW + 0.4f, 0.25f, 0.35f),
			EPFShellCollision::Cosmetic, MetalMaterial));
	}

	// Corner columns — visual weight at the building's four corners (like the arena's CornerCol*).
	const FVector FacadeCols[4] = {
		FVector(-50.f, FacadeS - 50.f, FacadeWallH * 0.5f),
		FVector(FieldX + 50.f, FacadeS - 50.f, FacadeWallH * 0.5f),
		FVector(-50.f, FacadeN + 50.f, FacadeWallH * 0.5f),
		FVector(FieldX + 50.f, FacadeN + 50.f, FacadeWallH * 0.5f),
	};
	for (int32 i = 0; i < 4; ++i)
	{
		DressingParts.Add(MakeShapePart(FString::Printf(TEXT("FacadeCol%d"), i),
			FacadeCols[i], FVector(0.7f, 0.7f, 18.5f),
			EPFShellCollision::Cosmetic, MetalMaterial, FRotator::ZeroRotator, true));
	}
}

void APFYardShell::BuildDesert()
{
	// ---- Sand ground plane: 90000×90000, top at Z = -15 (field pad top is 0, so the concrete
	// reads as a slab poured 15 uu proud of the sand). Solid: pawns walk on it, BBs splat, and
	// nothing can ever reach the KillZ inside a match. One giant cube — cheap and seamless. ----
	{
		UStaticMeshComponent* Sand = MakeShapePart(TEXT("DesertGround"),
			FVector(FieldX * 0.5f, FieldY * 0.5f, -40.f), FVector(900.f, 900.f, 0.5f),
			EPFShellCollision::Solid, SandBaseMaterial);
		SandParts.Add(Sand);
		SandTints.Add(FLinearColor(0.72f, 0.62f, 0.45f));   // dry sand
	}

	// ---- Distant landscape: mesas + dunes ringing the horizon 22–38k uu out. Deterministic
	// table (ctor-built geometry must be identical on every machine — no randomness). The height
	// fog (density 0.008, start 1000) hazes them into a believable desert distance. North forms
	// sit far enough back to rise BEHIND the warehouse next door. bCastShadow=false: at this
	// range shadows cost draw time and read as nothing. ----
	struct FDesertForm { float X, Y, SX, SY, SZ, Yaw; bool bDune; };
	const FDesertForm Forms[] = {
		// South horizon (behind the pen side)
		{  3200.f, -26000.f, 140.f,  60.f, 16.f,  12.f, false },   // broad mesa
		{ -9000.f, -21000.f,  90.f,  50.f, 11.f, -20.f, false },
		{ 16000.f, -23000.f, 110.f,  70.f, 13.f,  35.f, false },
		{  8000.f, -18000.f, 160.f,  90.f,  5.f,   8.f, true  },   // low dune
		// West horizon
		{ -24000.f,  2000.f,  70.f, 120.f, 14.f,  75.f, false },
		{ -28000.f, 12000.f, 100.f,  60.f, 18.f, -60.f, false },
		{ -19000.f, -6000.f, 130.f,  80.f,  4.f,  30.f, true  },
		// East horizon
		{ 30000.f,  6000.f,  90.f, 140.f, 15.f, -80.f, false },
		{ 25000.f, 16000.f, 120.f,  70.f, 10.f,  50.f, false },
		{ 22000.f, -4000.f, 150.f,  90.f,  4.5f, -15.f, true  },
		// North horizon — beyond the warehouse (facade back is ~FieldY+4200)
		{  1000.f, 30000.f, 150.f,  80.f, 17.f,  25.f, false },
		{ 12000.f, 26000.f, 100.f,  60.f, 12.f, -40.f, false },
		{ -7000.f, 24000.f, 170.f, 100.f,  5.f,  60.f, true  },
	};
	const FLinearColor MesaTint(0.60f, 0.47f, 0.34f);   // baked rock
	const FLinearColor DuneTint(0.76f, 0.66f, 0.48f);   // brighter drift sand
	int32 FormIdx = 0;
	for (const FDesertForm& F : Forms)
	{
		// Sit each form ON the sand plane (top of sand = -15; cube center = half its height up).
		UStaticMeshComponent* Form = MakeShapePart(
			FString::Printf(TEXT("DesertForm%d"), FormIdx++),
			FVector(F.X, F.Y, -15.f + F.SZ * 50.f),
			FVector(F.SX, F.SY, F.SZ),
			EPFShellCollision::Cosmetic, SandBaseMaterial,
			FRotator(0.f, F.Yaw, 0.f), /*bCastShadow=*/false);
		SandParts.Add(Form);
		SandTints.Add(F.bDune ? DuneTint : MesaTint);
	}
}
