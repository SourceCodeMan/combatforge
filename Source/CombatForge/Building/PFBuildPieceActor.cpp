// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFBuildPieceActor.h"

#include "CombatForge.h"
#include "Building/PFBuildPieceVisuals.h"
#include "Building/PFGridMath.h"
#include "Combat/PFHealthComponent.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"
#include "Player/CombatForgeCharacter.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	constexpr float Sub = 100.f; // PFGrid::SubUU
	constexpr float Cell = 400.f;
	constexpr float WallH = 300.f;
	constexpr float WallThick = 20.f;

	// Large window opening (see + shoot through both ways).
	// Prefixed names — unity builds share this TU with PFYardShell which also uses DoorW/DoorH.
	constexpr float PieceWinW = 280.f;
	constexpr float PieceWinH = 170.f;
	constexpr float PieceWinSill = 70.f;   // bottom of opening

	// Door opening (walk-through).
	constexpr float PieceDoorW = 140.f;
	constexpr float PieceDoorH = 230.f;
	constexpr float PieceDoorThick = 12.f;
}

APFBuildPieceActor::APFBuildPieceActor()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		CubeMesh = CubeFinder.Object;
	}
}

void APFBuildPieceActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APFBuildPieceActor, PieceId);
	DOREPLIFETIME(APFBuildPieceActor, PieceType);
	DOREPLIFETIME(APFBuildPieceActor, GridX);
	DOREPLIFETIME(APFBuildPieceActor, GridY);
	DOREPLIFETIME(APFBuildPieceActor, GridZ);
	DOREPLIFETIME(APFBuildPieceActor, GridRot);
	DOREPLIFETIME(APFBuildPieceActor, TeamId);
	DOREPLIFETIME(APFBuildPieceActor, bOpen);
	DOREPLIFETIME(APFBuildPieceActor, bOneWaySealed);
}

void APFBuildPieceActor::BeginPlay()
{
	Super::BeginPlay();
	EnsureGeometryBuilt();
}

void APFBuildPieceActor::InitFromRecord(const FPFBuildPieceRec& Rec)
{
	PieceId = Rec.PieceId;
	PieceType = Rec.Type;
	GridX = Rec.X;
	GridY = Rec.Y;
	GridZ = Rec.Z;
	GridRot = Rec.Rot;
	TeamId = Rec.Team;
	bOpen = false;
	bOneWaySealed = false;
	DoorYawAlpha = 0.f;
	DoorOpenRemaining = 0.f;
	TrapOpenRemaining = 0.f;

	const float Wx = GridX * Sub;
	const float Wy = GridY * Sub;
	const float Wz = GridZ * Sub;
	SetActorLocation(FVector(Wx, Wy, Wz));

	RebuildGeometry();
	ApplyOpenState();
	ForceNetUpdate();
}

void APFBuildPieceActor::OnRep_Open()
{
	EnsureGeometryBuilt();
	ApplyOpenState();
}

void APFBuildPieceActor::OnRep_OneWaySealed()
{
	EnsureGeometryBuilt();
	ApplyOpenState();
}

void APFBuildPieceActor::OnRep_PieceMeta()
{
	// Always rebuild: type/rot can arrive after a partial first build, and clients never inherit
	// the server's NewObject mesh comps (those don't replicate).
	for (UStaticMeshComponent* P : SolidParts)
	{
		if (IsValid(P))
		{
			P->DestroyComponent();
		}
	}
	SolidParts.Reset();
	DoorLeaf = nullptr;
	OneWaySealPlate = nullptr;
	TrapPlate = nullptr;
	EnsureGeometryBuilt();
}

void APFBuildPieceActor::EnsureGeometryBuilt()
{
	if (PieceId == 0)
	{
		return;
	}
	if (SolidParts.Num() > 0)
	{
		return;
	}
	const float Wx = GridX * Sub;
	const float Wy = GridY * Sub;
	const float Wz = GridZ * Sub;
	SetActorLocation(FVector(Wx, Wy, Wz));
	RebuildGeometry();
	ApplyOpenState();
}

void APFBuildPieceActor::ApplyCollisionPreset(UPrimitiveComponent* Comp, bool bBlock)
{
	if (!Comp)
	{
		return;
	}
	if (!bBlock)
	{
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}
	Comp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Comp->SetCollisionObjectType(ECC_WorldStatic);
	Comp->SetCollisionResponseToAllChannels(ECR_Ignore);
	Comp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Comp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	Comp->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Comp->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	Comp->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
	Comp->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Block);
	Comp->SetCanEverAffectNavigation(true);
}

UStaticMeshComponent* APFBuildPieceActor::AddCubePart(const FName& Name, const FVector& WorldCenter,
	const FVector& WorldExtent, const FRotator& WorldRot, UMaterialInterface* Mat, bool bBlock)
{
	UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(this, Name);
	Comp->SetupAttachment(Root);
	Comp->SetStaticMesh(CubeMesh);
	Comp->SetMobility(EComponentMobility::Movable);
	Comp->SetCastShadow(true);
	Comp->RegisterComponent();
	// Engine cube is 100³; scale so full size = WorldExtent*2.
	const FVector Scale(
		(WorldExtent.X * 2.f) / 100.f,
		(WorldExtent.Y * 2.f) / 100.f,
		(WorldExtent.Z * 2.f) / 100.f);
	Comp->SetWorldLocation(WorldCenter);
	Comp->SetWorldRotation(WorldRot);
	Comp->SetWorldScale3D(Scale);
	if (Mat)
	{
		Comp->SetMaterial(0, Mat);
	}
	ApplyCollisionPreset(Comp, bBlock);
	SolidParts.Add(Comp);
	return Comp;
}

FVector APFBuildPieceActor::WallFrontNormal() const
{
	// N edge (0): front = +Y; E edge (1): front = +X.
	return (GridRot == 1) ? FVector(1.f, 0.f, 0.f) : FVector(0.f, 1.f, 0.f);
}

void APFBuildPieceActor::RebuildGeometry()
{
	// Tear down prior parts (re-init / client rep).
	for (UStaticMeshComponent* P : SolidParts)
	{
		if (IsValid(P))
		{
			P->DestroyComponent();
		}
	}
	SolidParts.Reset();
	DoorLeaf = nullptr;
	OneWaySealPlate = nullptr;
	TrapPlate = nullptr;
	if (TrapTrigger)
	{
		TrapTrigger->DestroyComponent();
		TrapTrigger = nullptr;
	}

	PFBuildPieceVisuals::EnsureLoaded();
	FrameMID = PFBuildPieceVisuals::CreateStructuralPaletteMID(this, EPFPieceType::Wall);
	DoorMID = PFBuildPieceVisuals::CreateStructuralPaletteMID(this, EPFPieceType::Ramp); // metal door
	TrapMID = PFBuildPieceVisuals::CreateStructuralPaletteMID(this, EPFPieceType::Floor);
	// Cheap translucent pane: BasicShape with low opacity if possible.
	if (UMaterialInterface* Basic = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")).TryLoad()))
	{
		PaneMID = UMaterialInstanceDynamic::Create(Basic, this);
		if (PaneMID)
		{
			PaneMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.55f, 0.75f, 0.95f, 0.25f));
		}
	}

	switch (PieceType)
	{
	case EPFPieceType::WallWindow:
		BuildWallFrameParts(/*door=*/false, /*window=*/true);
		break;
	case EPFPieceType::WallDoor:
		BuildWallFrameParts(/*door=*/true, /*window=*/false);
		BuildDoorLeaf();
		break;
	case EPFPieceType::WallDoorOneWay:
		// Same single-thickness door as a normal door; seal plate is flush and only after first use.
		BuildWallFrameParts(/*door=*/true, /*window=*/false);
		BuildDoorLeaf();
		BuildOneWaySealPlate();
		break;
	case EPFPieceType::FloorTrap:
		BuildTrapFloor();
		break;
	default:
		break;
	}
}

void APFBuildPieceActor::BuildWallFrameParts(bool bWithDoorOpening, bool bWithWindowOpening)
{
	const float Wx = GridX * Sub;
	const float Wy = GridY * Sub;
	const float Wz = GridZ * Sub;
	const bool bEast = (GridRot == 1);

	// Wall plane centerline.
	const float Along0 = 0.f;
	const float Along1 = Cell;
	const float MidAlong = Cell * 0.5f;
	const float ThickHalf = WallThick * 0.5f;

	auto AlongPos = [&](float Along) -> FVector
	{
		if (bEast)
		{
			return FVector(Wx + Cell, Wy + Along, 0.f);
		}
		return FVector(Wx + Along, Wy + Cell, 0.f);
	};
	auto ExtentAlong = [&](float AlongHalf, float ZHalf) -> FVector
	{
		// Extent in world axes (half-size).
		if (bEast)
		{
			return FVector(ThickHalf, AlongHalf, ZHalf);
		}
		return FVector(AlongHalf, ThickHalf, ZHalf);
	};
	auto Center = [&](float Along, float Zc) -> FVector
	{
		const FVector P = AlongPos(Along);
		return FVector(P.X, P.Y, Wz + Zc);
	};

	if (bWithWindowOpening)
	{
		// Left / right posts + sill + header around a large center hole.
		const float SideW = (Cell - PieceWinW) * 0.5f; // 60
		const float PostAlongHalf = SideW * 0.5f;
		const float LeftAlong = SideW * 0.5f;
		const float RightAlong = Cell - SideW * 0.5f;
		const float MidZ = WallH * 0.5f;

		AddCubePart(TEXT("WinPostL"), Center(LeftAlong, MidZ), ExtentAlong(PostAlongHalf, MidZ),
			FRotator::ZeroRotator, FrameMID, true);
		AddCubePart(TEXT("WinPostR"), Center(RightAlong, MidZ), ExtentAlong(PostAlongHalf, MidZ),
			FRotator::ZeroRotator, FrameMID, true);

		const float SillZ = PieceWinSill * 0.5f;
		const float HeaderZ = PieceWinSill + PieceWinH + (WallH - PieceWinSill - PieceWinH) * 0.5f;
		const float HeaderH = (WallH - PieceWinSill - PieceWinH) * 0.5f;
		const float OpenAlongHalf = PieceWinW * 0.5f;

		AddCubePart(TEXT("WinSill"), Center(MidAlong, SillZ), ExtentAlong(OpenAlongHalf, SillZ),
			FRotator::ZeroRotator, FrameMID, true);
		AddCubePart(TEXT("WinHeader"), Center(MidAlong, HeaderZ), ExtentAlong(OpenAlongHalf, HeaderH),
			FRotator::ZeroRotator, FrameMID, true);

		// No solid "glass" fill: BasicShapeMaterial is opaque, so a pane mesh read as a solid wall
		// and hid the opening. Empty hole + frame = see/shoot-through window.
		return;
	}

	if (bWithDoorOpening)
	{
		// Side posts + header; walk-through opening at center bottom.
		const float SideW = (Cell - PieceDoorW) * 0.5f; // 130
		const float PostAlongHalf = SideW * 0.5f;
		const float LeftAlong = SideW * 0.5f;
		const float RightAlong = Cell - SideW * 0.5f;
		const float MidZ = WallH * 0.5f;

		AddCubePart(TEXT("DoorPostL"), Center(LeftAlong, MidZ), ExtentAlong(PostAlongHalf, MidZ),
			FRotator::ZeroRotator, FrameMID, true);
		AddCubePart(TEXT("DoorPostR"), Center(RightAlong, MidZ), ExtentAlong(PostAlongHalf, MidZ),
			FRotator::ZeroRotator, FrameMID, true);

		const float HeaderH = (WallH - PieceDoorH) * 0.5f;
		const float HeaderZ = PieceDoorH + HeaderH;
		AddCubePart(TEXT("DoorHeader"), Center(MidAlong, HeaderZ),
			ExtentAlong(PieceDoorW * 0.5f, HeaderH), FRotator::ZeroRotator, FrameMID, true);
		return;
	}
}

void APFBuildPieceActor::BuildDoorLeaf()
{
	DoorLeaf = NewObject<UStaticMeshComponent>(this, TEXT("DoorLeaf"));
	DoorLeaf->SetupAttachment(Root);
	DoorLeaf->SetStaticMesh(CubeMesh);
	DoorLeaf->SetMobility(EComponentMobility::Movable);
	DoorLeaf->SetCastShadow(true);
	DoorLeaf->RegisterComponent();
	if (DoorMID)
	{
		DoorLeaf->SetMaterial(0, DoorMID);
	}
	// Scale: door size 140 x 12 x 230 → extents
	const FVector Scale(PieceDoorW / 100.f, PieceDoorThick / 100.f, PieceDoorH / 100.f);
	DoorLeaf->SetWorldScale3D(Scale);
	DoorLeaf->SetWorldLocation(DoorLeafClosedCenter());
	DoorLeaf->SetWorldRotation(DoorLeafClosedRotation());
	ApplyCollisionPreset(DoorLeaf, true);
	SolidParts.Add(DoorLeaf);
}

FVector APFBuildPieceActor::DoorLeafClosedCenter() const
{
	const float Wx = GridX * Sub;
	const float Wy = GridY * Sub;
	const float Wz = GridZ * Sub;
	const float MidAlong = Cell * 0.5f;
	const float Zc = PieceDoorH * 0.5f;
	if (GridRot == 1)
	{
		return FVector(Wx + Cell, Wy + MidAlong, Wz + Zc);
	}
	return FVector(Wx + MidAlong, Wy + Cell, Wz + Zc);
}

FRotator APFBuildPieceActor::DoorLeafClosedRotation() const
{
	// Match wall plane: N wall → default; E wall → 90 yaw.
	return (GridRot == 1) ? FRotator(0.f, 90.f, 0.f) : FRotator::ZeroRotator;
}

FRotator APFBuildPieceActor::DoorLeafOpenRotation() const
{
	// Swing open ~100° toward the front side.
	const float Base = (GridRot == 1) ? 90.f : 0.f;
	return FRotator(0.f, Base + 100.f, 0.f);
}

void APFBuildPieceActor::BuildOneWaySealPlate()
{
	// Flush fill of the door aperture in the SAME wall plane as the door leaf — not an offset second
	// wall (that was double-thickness and gave the trick away). Starts hidden until first seal.
	const FVector Center = DoorLeafClosedCenter();
	const float ThickHalf = WallThick * 0.5f;
	FVector Ext;
	if (GridRot == 1)
	{
		// E wall: thickness along X, door width along Y.
		Ext = FVector(ThickHalf, PieceDoorW * 0.5f, PieceDoorH * 0.5f);
	}
	else
	{
		// N wall: thickness along Y, door width along X.
		Ext = FVector(PieceDoorW * 0.5f, ThickHalf, PieceDoorH * 0.5f);
	}

	OneWaySealPlate = AddCubePart(TEXT("OneWaySeal"), Center, Ext,
		DoorLeafClosedRotation(), FrameMID, /*bBlock=*/false);
	if (OneWaySealPlate)
	{
		OneWaySealPlate->SetVisibility(false);
	}
}

void APFBuildPieceActor::BuildTrapFloor()
{
	const float Wx = GridX * Sub;
	const float Wy = GridY * Sub;
	const float Wz = GridZ * Sub;
	// Same footprint as a floor: 400×400×20, top at level Z.
	const FVector Center(Wx + 200.f, Wy + 200.f, Wz - 10.f);
	const FVector Ext(200.f, 200.f, 10.f);
	TrapPlate = AddCubePart(TEXT("TrapPlate"), Center, Ext, FRotator::ZeroRotator, TrapMID, true);

	// Slightly darker trap cue.
	if (TrapMID)
	{
		TrapMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.55f, 0.2f, 0.15f, 1.f));
	}

	TrapTrigger = NewObject<UBoxComponent>(this, TEXT("TrapTrigger"));
	TrapTrigger->SetupAttachment(Root);
	TrapTrigger->SetBoxExtent(FVector(190.f, 190.f, 40.f));
	TrapTrigger->SetWorldLocation(FVector(Wx + 200.f, Wy + 200.f, Wz + 40.f));
	TrapTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	TrapTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	TrapTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	TrapTrigger->SetGenerateOverlapEvents(true);
	TrapTrigger->RegisterComponent();
}

void APFBuildPieceActor::ApplyOpenState()
{
	const bool bDoorType = (PieceType == EPFPieceType::WallDoor || PieceType == EPFPieceType::WallDoorOneWay);
	// One-way after first open→close: closed aperture shows a flush wall plate instead of the door leaf.
	const bool bShowOneWaySeal = (PieceType == EPFPieceType::WallDoorOneWay) && bOneWaySealed && !bOpen;

	if (bDoorType && DoorLeaf)
	{
		// Collision off while open so pawns pass; sealed-closed uses the flush plate instead of the leaf.
		ApplyCollisionPreset(DoorLeaf, !bOpen && !bShowOneWaySeal);
		DoorLeaf->SetVisibility(!bShowOneWaySeal);
		if (bOpen)
		{
			DoorLeaf->SetWorldRotation(DoorLeafOpenRotation());
			DoorYawAlpha = 1.f;
		}
		else
		{
			DoorLeaf->SetWorldRotation(DoorLeafClosedRotation());
			DoorYawAlpha = 0.f;
		}
	}

	if (PieceType == EPFPieceType::WallDoorOneWay && OneWaySealPlate)
	{
		// Seal only after first use, only while closed, flush in the door plane (single thickness).
		// While open the hole is clear — walk through from either side.
		ApplyCollisionPreset(OneWaySealPlate, bShowOneWaySeal);
		OneWaySealPlate->SetVisibility(bShowOneWaySeal);
	}

	if (PieceType == EPFPieceType::FloorTrap && TrapPlate)
	{
		ApplyCollisionPreset(TrapPlate, !bOpen);
		TrapPlate->SetVisibility(!bOpen);
	}
}

bool APFBuildPieceActor::CanUserToggleDoor(const APawn* User) const
{
	if (!User || (PieceType != EPFPieceType::WallDoor && PieceType != EPFPieceType::WallDoorOneWay))
	{
		return false;
	}
	const FVector Me = User->GetActorLocation();
	const FVector DoorCenter = DoorLeafClosedCenter();
	if (FVector::DistSquared(Me, DoorCenter) > FMath::Square(DoorInteractRangeUU))
	{
		return false;
	}
	if (PieceType == EPFPieceType::WallDoorOneWay)
	{
		// Front side only — the trick is who can open it, not a second wall layer.
		const FVector ToUser = (Me - DoorCenter).GetSafeNormal2D();
		if (FVector::DotProduct(ToUser, WallFrontNormal()) < 0.15f)
		{
			return false;
		}
	}
	return true;
}

void APFBuildPieceActor::AuthorityTryToggleDoor(APawn* User)
{
	if (!HasAuthority() || !CanUserToggleDoor(User))
	{
		return;
	}
	if (const ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(User))
	{
		if (const UPFHealthComponent* H = Char->GetHealth(); H && H->bEliminated)
		{
			return;
		}
	}
	if (bOpen)
	{
		// Manual close (or toggle from open) — clear auto-close timer and seal one-ways.
		AuthorityCloseDoor();
	}
	else
	{
		bOpen = true;
		DoorOpenRemaining = DoorOpenSeconds;
		ApplyOpenState();
		ForceNetUpdate();
	}
	UE_LOG(CombatForgeLog, Log, TEXT("Door piece %u %s by %s%s"),
		PieceId, bOpen ? TEXT("OPEN") : TEXT("CLOSE"), *GetNameSafe(User),
		(PieceType == EPFPieceType::WallDoorOneWay && bOneWaySealed) ? TEXT(" (one-way sealed)") : TEXT(""));
}

void APFBuildPieceActor::AuthorityCloseDoor()
{
	if (!HasAuthority() || !bOpen)
	{
		return;
	}
	bOpen = false;
	DoorOpenRemaining = 0.f;
	// First close after an open arms the flush seal (trick door → wall when closed thereafter).
	if (PieceType == EPFPieceType::WallDoorOneWay)
	{
		bOneWaySealed = true;
	}
	ApplyOpenState();
	ForceNetUpdate();
}

void APFBuildPieceActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (PieceType == EPFPieceType::FloorTrap && HasAuthority())
	{
		TickTrap(DeltaSeconds);
	}
	if (PieceType == EPFPieceType::WallDoor || PieceType == EPFPieceType::WallDoorOneWay)
	{
		if (HasAuthority())
		{
			TickDoorAutoClose(DeltaSeconds);
		}
		TickDoorAnim(DeltaSeconds);
	}
}

void APFBuildPieceActor::TickDoorAutoClose(float DeltaSeconds)
{
	if (!bOpen)
	{
		DoorOpenRemaining = 0.f;
		return;
	}
	DoorOpenRemaining -= DeltaSeconds;
	if (DoorOpenRemaining <= 0.f)
	{
		AuthorityCloseDoor();
		UE_LOG(CombatForgeLog, Verbose, TEXT("Door piece %u auto-closed after %.1fs"),
			PieceId, DoorOpenSeconds);
	}
}

void APFBuildPieceActor::TickDoorAnim(float DeltaSeconds)
{
	if (!DoorLeaf)
	{
		return;
	}
	const float Target = bOpen ? 1.f : 0.f;
	DoorYawAlpha = FMath::FInterpTo(DoorYawAlpha, Target, DeltaSeconds, 8.f);
	const FRotator Closed = DoorLeafClosedRotation();
	const FRotator Opened = DoorLeafOpenRotation();
	DoorLeaf->SetWorldRotation(FMath::Lerp(Closed, Opened, DoorYawAlpha));
	// Keep hinge near closed center (simple swing about center — good enough for graybox).
	DoorLeaf->SetWorldLocation(DoorLeafClosedCenter());
}

void APFBuildPieceActor::TickTrap(float DeltaSeconds)
{
	if (bOpen)
	{
		TrapOpenRemaining -= DeltaSeconds;
		if (TrapOpenRemaining <= 0.f)
		{
			bOpen = false;
			TrapOpenRemaining = 0.f;
			ApplyOpenState();
			ForceNetUpdate();
		}
		return;
	}

	// Trip only when an ENEMY of the placing team stands on the plate (owner team is safe).
	UWorld* World = GetWorld();
	if (!World || !TrapTrigger)
	{
		return;
	}
	const ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || !GS->IsFireAllowed())
	{
		return;   // combat-live only
	}

	const FVector Center = TrapTrigger->GetComponentLocation();
	const FVector Ext = TrapTrigger->GetScaledBoxExtent();
	const FBox TriggerBox(Center - Ext, Center + Ext);
	for (TActorIterator<ACharacter> It(World); It; ++It)
	{
		ACombatForgeCharacter* PFC = Cast<ACombatForgeCharacter>(*It);
		if (!PFC)
		{
			continue;
		}
		if (const UPFHealthComponent* H = PFC->GetHealth(); H && H->bEliminated)
		{
			continue;
		}
		const ACombatForgePlayerState* PS = PFC->GetPlayerState<ACombatForgePlayerState>();
		if (!PS)
		{
			continue;
		}
		// Placing team is immune. Anyone else (other team / FFA foe) trips the trap.
		if (PS->TeamId == TeamId)
		{
			continue;
		}
		const FVector Feet = PFC->GetActorLocation()
			- FVector(0.f, 0.f, PFC->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		if (!TriggerBox.IsInsideOrOn(Feet) && !TriggerBox.IsInsideOrOn(PFC->GetActorLocation()))
		{
			continue;
		}

		bOpen = true;
		TrapOpenRemaining = TrapOpenSeconds;
		ApplyOpenState();
		ForceNetUpdate();
		UE_LOG(CombatForgeLog, Log, TEXT("Trap floor %u tripped by enemy team %u (owner team %u)"),
			PieceId, static_cast<uint32>(PS->TeamId), static_cast<uint32>(TeamId));
		return;
	}
}
