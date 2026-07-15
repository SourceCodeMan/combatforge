// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFYardShell.h"

#include "Components/StaticMeshComponent.h"

APFYardShell::APFYardShell()
	: APFArenaShell(PFGetArenaMapDef(EPFArenaMap::Yard))
{
	// Base ctor already built the full 6400×8000 field (roofless — MapDef.bRoof gates the deck /
	// trusses / bay lights inside the shared dressing pass). Only the venue scenery is ours.
	BuildWarehouseFacade();
}

void APFYardShell::BuildWarehouseFacade()
{
	// The warehouse the Warehouse map plays INSIDE, seen from outside: a windowless cube-primitive
	// building filling Y ∈ [FieldY+200, FieldY+4200] across the full field length. NORTH edge
	// because the warm-up pen owns the south exterior (Y=-3000, base ctor). All parts Cosmetic —
	// the perimeter wall is the thing that blocks entry — and registered in DressingParts so the
	// cohesion palette pass (ApplyCohesivePalette) rebinds them like every other dressing cube.
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
		EPFShellCollision::Cosmetic, WallMaterial, FRotator::ZeroRotator, /*bCastShadow=*/true));
	DressingParts.Add(MakeShapePart(TEXT("FacadeWallN"),
		FVector(FieldX * 0.5f, FacadeN, FacadeWallH * 0.5f),
		FVector(FieldX / 100.f + 0.4f, 0.3f, FacadeWallH / 100.f),
		EPFShellCollision::Cosmetic, WallMaterial, FRotator::ZeroRotator, true));
	DressingParts.Add(MakeShapePart(TEXT("FacadeWallW"),
		FVector(0.f, FacadeMidY, FacadeWallH * 0.5f),
		FVector(0.3f, FacadeDepthScale + 0.4f, FacadeWallH / 100.f),
		EPFShellCollision::Cosmetic, WallMaterial, FRotator::ZeroRotator, true));
	DressingParts.Add(MakeShapePart(TEXT("FacadeWallE"),
		FVector(FieldX, FacadeMidY, FacadeWallH * 0.5f),
		FVector(0.3f, FacadeDepthScale + 0.4f, FacadeWallH / 100.f),
		EPFShellCollision::Cosmetic, WallMaterial, FRotator::ZeroRotator, true));

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
