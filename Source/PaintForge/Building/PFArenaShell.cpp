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
	constexpr float PerimeterH = static_cast<float>(PFGrid::HeightCapUU);           // 1200
	constexpr float SpawnZ = 100.f;            // capsule half-height + clearance over the Z=0 floor
	constexpr int32 MaxRosterSlots = 12;       // RosterIndex 0..11 (§3.2)

	// Warm-up pen: 2000×2000 uu, centered south of the field at Y = -3000 (T29).
	constexpr float PenCenterX = FieldX * 0.5f;   // 3200
	constexpr float PenCenterY = -3000.f;
	constexpr float PenHalf = 1000.f;
	constexpr float PenWallH = 300.f;          // full cover height — unjumpable (03 §1)

	constexpr float TeamSpawnSpacingY = FieldY / PFGrid::SpawnPointsPerTeam;   // 666.67
	constexpr float PenSlotSpacingX = 160.f;
	constexpr float PenSlotStartX = PenCenterX - PenSlotSpacingX * (MaxRosterSlots - 1) * 0.5f;
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
	static ConstructorHelpers::FObjectFinder<UMaterial>   MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CubeMesh = CubeFinder.Object;
	ShapeMaterial = MaterialFinder.Object;

	// --- Field floor slab: 6400×4000×30, top at Z = 0 (T25: level-0 floors sit flush inside it) ---
	FieldFloor = MakeShapePart(TEXT("FieldFloor"),
		FVector(FieldX * 0.5f, FieldY * 0.5f, -15.f), FVector(64.f, 40.f, 0.3f),
		EPFShellCollision::SolidBuildable);

	// --- 4 perimeter walls, h = 1200, hugging the field bounds (slightly long to close corners) ---
	PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallN"),
		FVector(FieldX * 0.5f, FieldY + 10.f, PerimeterH * 0.5f), FVector(64.4f, 0.2f, 12.f),
		EPFShellCollision::Solid));
	PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallS"),
		FVector(FieldX * 0.5f, -10.f, PerimeterH * 0.5f), FVector(64.4f, 0.2f, 12.f),
		EPFShellCollision::Solid));
	PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallW"),
		FVector(-10.f, FieldY * 0.5f, PerimeterH * 0.5f), FVector(0.2f, 40.4f, 12.f),
		EPFShellCollision::Solid));
	PerimeterWalls.Add(MakeShapePart(TEXT("PerimeterWallE"),
		FVector(FieldX + 10.f, FieldY * 0.5f, PerimeterH * 0.5f), FVector(0.2f, 40.4f, 12.f),
		EPFShellCollision::Solid));

	// --- Team-tinted spawn-strip floor tiles over the spawn columns (x = 0 and x = 15) ---
	SpawnStrips.Add(MakeShapePart(TEXT("SpawnStripA"),
		FVector(PFGrid::CellUU * 0.5f, FieldY * 0.5f, 1.f), FVector(4.f, 40.f, 0.02f),
		EPFShellCollision::Cosmetic));
	SpawnStrips.Add(MakeShapePart(TEXT("SpawnStripB"),
		FVector(FieldX - PFGrid::CellUU * 0.5f, FieldY * 0.5f, 1.f), FVector(4.f, 40.f, 0.02f),
		EPFShellCollision::Cosmetic));

	// --- Midline (T23): 40 uu gray posts every 400 uu + painted floor stripe; occludes nothing ---
	const float MidX = FieldX * 0.5f;   // 3200, center of the neutral strip (cells 7..8)
	for (int32 PostIdx = 0; PostIdx <= PFGrid::CellsY; ++PostIdx)
	{
		MidlinePosts.Add(MakeShapePart(FString::Printf(TEXT("MidlinePost%d"), PostIdx),
			FVector(MidX, PostIdx * PFGrid::CellUU, PerimeterH * 0.5f), FVector(0.4f, 0.4f, 12.f),
			EPFShellCollision::Solid));
	}
	MidlineStripe = MakeShapePart(TEXT("MidlineStripe"),
		FVector(MidX, FieldY * 0.5f, 1.5f), FVector(0.4f, 40.f, 0.02f),
		EPFShellCollision::Cosmetic);

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
		EPFShellCollision::Solid);
	PenWalls.Add(MakeShapePart(TEXT("PenWallN"),
		FVector(PenCenterX, PenCenterY + PenHalf - 10.f, PenWallH * 0.5f), FVector(20.f, 0.2f, 3.f),
		EPFShellCollision::Solid));
	PenWalls.Add(MakeShapePart(TEXT("PenWallS"),
		FVector(PenCenterX, PenCenterY - PenHalf + 10.f, PenWallH * 0.5f), FVector(20.f, 0.2f, 3.f),
		EPFShellCollision::Solid));
	PenWalls.Add(MakeShapePart(TEXT("PenWallW"),
		FVector(PenCenterX - PenHalf + 10.f, PenCenterY, PenWallH * 0.5f), FVector(0.2f, 20.f, 3.f),
		EPFShellCollision::Solid));
	PenWalls.Add(MakeShapePart(TEXT("PenWallE"),
		FVector(PenCenterX + PenHalf - 10.f, PenCenterY, PenWallH * 0.5f), FVector(0.2f, 20.f, 3.f),
		EPFShellCollision::Solid));
}

UStaticMeshComponent* APFArenaShell::MakeShapePart(const FString& Name, const FVector& Center,
                                                   const FVector& Scale, EPFShellCollision Mode)
{
	UStaticMeshComponent* Comp = CreateDefaultSubobject<UStaticMeshComponent>(*Name);
	Comp->SetupAttachment(ShellRoot);
	Comp->SetMobility(EComponentMobility::Static);
	Comp->SetStaticMesh(CubeMesh);
	Comp->SetMaterial(0, ShapeMaterial);
	Comp->SetRelativeLocation(Center);
	Comp->SetRelativeScale3D(Scale);
	Comp->SetCastShadow(false);
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
		ApplyTint(SpawnStrips[0], PFColors::ForTeam(0));
		ApplyTint(SpawnStrips[1], PFColors::ForTeam(1));
	}
	const FLinearColor MidGray(0.35f, 0.35f, 0.38f);
	ApplyTint(MidlineStripe, MidGray);
	for (UStaticMeshComponent* Post : MidlinePosts)
	{
		ApplyTint(Post, MidGray);
	}

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

void APFArenaShell::ApplyTint(UStaticMeshComponent* Comp, const FLinearColor& Color)
{
	if (!Comp || !ShapeMaterial)
	{
		return;
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ShapeMaterial, this);
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
	// Just inside the plot's spawn-side edge (cells 1-2 / 13-14), looking down-plot at the midline.
	const float X = bTeamA ? 2.f * PFGrid::CellUU : FieldX - 2.f * PFGrid::CellUU;
	const float Y = (Slot + 0.5f) * TeamSpawnSpacingY;
	const float Yaw = bTeamA ? 0.f : 180.f;
	return FTransform(FRotator(0.f, Yaw, 0.f), FVector(X, Y, SpawnZ));
}

FTransform APFArenaShell::GetWarmupSpawnTransform(int32 SlotIdx) const
{
	const int32 Slot = FMath::Clamp(SlotIdx, 0, MaxRosterSlots - 1);
	const float X = PenSlotStartX + Slot * PenSlotSpacingX;
	const float Y = PenCenterY - PenHalf + 300.f;   // south line, facing the dummies to the north
	return FTransform(FRotator(0.f, 90.f, 0.f), FVector(X, Y, SpawnZ));
}

FTransform APFArenaShell::GetWarmupDummyTransform(int32 SlotIdx) const
{
	const int32 Slot = FMath::Clamp(SlotIdx, 0, MaxRosterSlots - 1);
	const float X = PenSlotStartX + Slot * PenSlotSpacingX;
	const float Y = PenCenterY + PenHalf - 300.f;   // north line, facing the players
	return FTransform(FRotator(0.f, -90.f, 0.f), FVector(X, Y, 90.f));   // dummy body r≈40 h≈180
}
