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
	// STATIC root (regression fix, Tom's 2026-08-07 solo test): the frame parts are Static (see
	// AddCubePart) and UE REFUSES to attach a Static child to a Movable parent — every window's
	// WinSill/WinPost attach aborted and windows placed as nothing. This actor never moves after
	// spawn; the spawner passes the final grid transform (SpawnSpecialPieceActor), clients get it
	// from the spawn bunch, and part geometry is authored in WORLD space regardless.
	Root->SetMobility(EComponentMobility::Static);
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

	// No SetActorLocation here: the root is STATIC (ctor) and refuses post-spawn moves. The
	// server spawns this actor at its grid transform (SpawnSpecialPieceActor) and clients place
	// it from the spawn bunch; all part geometry below is computed in world space from GridX/Y/Z.
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
	// No SetActorLocation (static root; parts are world-space from GridX/Y/Z — see Init note).
	RebuildGeometry();
	ApplyOpenState();
}

void APFBuildPieceActor::ApplyCollisionPreset(UPrimitiveComponent* Comp, bool bBlock, bool bAffectNav)
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
	Comp->SetCanEverAffectNavigation(bAffectNav);
}

// A NORMAL door leaf must NOT carve the navmesh: if it does, the closed door is a nav obstacle, the
// pathfinder routes bots AROUND it, and they never approach/stall/open it (Tom: "make bots open doors").
// Nav-transparent + still-blocking means the runtime navmesh threads through the ~140uu doorway, bots path
// in, their capsule stalls on the physical leaf, and the stall→AuthorityTryToggleDoor loop fires.
// A one-way door KEEPS carving nav (bAffectNav stays true) so the pathfinder still routes around it —
// that preserves the one-way trick (a bot can't be routed the wrong way through it and get stuck).
bool APFBuildPieceActor::DoorLeafAffectsNav() const
{
	return PieceType != EPFPieceType::WallDoor;
}

UStaticMeshComponent* APFBuildPieceActor::AddCubePart(const FName& Name, const FVector& WorldCenter,
	const FVector& WorldExtent, const FRotator& WorldRot, UMaterialInterface* Mat, bool bBlock)
{
	// UNIQUE name (playtest 2026-08-06 log forensics): net-recycled actor names re-ran this with the
	// SAME fixed part names while the old components were still pending cleanup — 11k+ "Gamethread
	// hitch waiting for resource cleanup on a UObject ... overwrite" per client session. A unique
	// suffix keeps the readable stem and never overwrites a dying object.
	UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(this,
		MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), Name));
	Comp->SetupAttachment(Root);
	Comp->SetStaticMesh(CubeMesh);
	// STATIC, not Movable (playtest 2026-08-06): these parts never move after spawn, but Movable
	// mobility made pawns standing on them (sill / trap plate / posts) use RELATIVE based movement,
	// and since runtime NewObject components are not stably named they cannot get a NetGUID — every
	// server position correction against such a base was IGNORED on the client (~5k+ per session:
	// "ClientAdjustPosition could not resolve the new relative movement base actor"). Ignored
	// corrections are accumulating client/server position desync — rubber-banding, shots landing
	// where players are not. Static bases use ABSOLUTE corrections, which need no base resolution.
	// The transform must therefore be final BEFORE registration (Static refuses moves afterwards).
	Comp->SetMobility(EComponentMobility::Static);
	Comp->SetCastShadow(true);
	// Engine cube is 100³; scale so full size = WorldExtent*2.
	const FVector Scale(
		(WorldExtent.X * 2.f) / 100.f,
		(WorldExtent.Y * 2.f) / 100.f,
		(WorldExtent.Z * 2.f) / 100.f);
	Comp->SetWorldLocation(WorldCenter);
	Comp->SetWorldRotation(WorldRot);
	Comp->SetWorldScale3D(Scale);
	Comp->RegisterComponent();
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
	// Unique name for the same overwrite-hitch reason as AddCubePart. Stays MOVABLE — it swings.
	DoorLeaf = NewObject<UStaticMeshComponent>(this,
		MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), TEXT("DoorLeaf")));
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
	ApplyCollisionPreset(DoorLeaf, true, DoorLeafAffectsNav());
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

	TrapTrigger = NewObject<UBoxComponent>(this,
		MakeUniqueObjectName(this, UBoxComponent::StaticClass(), TEXT("TrapTrigger")));
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
		// Normal doors stay nav-transparent (bots path through + open); one-way keeps carving nav.
		ApplyCollisionPreset(DoorLeaf, !bOpen && !bShowOneWaySeal, DoorLeafAffectsNav());
		DoorLeaf->SetVisibility(!bShowOneWaySeal);
		// DoorYawAlpha / rotation are NOT set here — TickDoorAnim interpolates the alpha toward the bOpen
		// target every frame and drives the rotation from it, so the leaf visibly SWINGS open and shut.
		// The old code hard-snapped alpha to 0/1 here, giving the interp zero travel: the door teleported
		// between states, and on a one-way that read as "snaps straight back to a wall". BuildDoorLeaf sets
		// the initial closed rotation, so a freshly spawned closed door still looks right before the first tick.
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
		bOneWaySealPending = false;   // a reopen mid-close-swing cancels the queued seal (next close re-arms it)
		ApplyOpenState();
		ForceNetUpdate();
	}
	// bOneWaySealed is dropped for the swing on the close path, so log the PENDING seal instead.
	UE_LOG(CombatForgeLog, Log, TEXT("Door piece %u %s by %s%s"),
		PieceId, bOpen ? TEXT("OPEN") : TEXT("CLOSE"), *GetNameSafe(User),
		(PieceType == EPFPieceType::WallDoorOneWay && bOneWaySealPending) ? TEXT(" (one-way seal pending)") : TEXT(""));
}

void APFBuildPieceActor::AuthorityCloseDoor()
{
	if (!HasAuthority() || !bOpen)
	{
		return;
	}
	bOpen = false;
	DoorOpenRemaining = 0.f;
	// One-way trick door: the seal (leaf → flush wall) is DEFERRED until the close swing finishes, so the
	// leaf visibly swings shut first. Applies to EVERY close, including re-closing an already-sealed door
	// that a front-side player reopened — bShowOneWaySeal keys on bOneWaySealed && !bOpen, so a re-close
	// with the seal still set would hide the leaf and pop the wall in instantly (the "flashes open then
	// turns to a wall" report). Drop the seal for the duration of the swing; TickDoorAnim re-arms it at
	// the end. (The original bug was setting bOneWaySealed=true on this same frame — no swing at all.)
	if (PieceType == EPFPieceType::WallDoorOneWay)
	{
		bOneWaySealed = false;
		bOneWaySealPending = true;
	}
	ApplyOpenState();      // bOpen just went false → leaf stays visible + solid and TickDoorAnim swings it shut
	ForceNetUpdate();      // replicate bOpen (and a dropped seal) so clients start their own close swing
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

	// One-way seal completion: once the close swing has fully landed ON THE SERVER, solidify into the flush
	// wall plate. bOneWaySealed replicates (OnRep_OneWaySealed → ApplyOpenState) and the client applies it on
	// ARRIVAL — there is no client-side alpha gate. That's fine in practice: the client's swing also started
	// one half-RTT late (OnRep_Open), so the two delays cancel and its leaf is within frame-jitter of closed
	// when the plate pops in.
	if (bOneWaySealPending && HasAuthority() && !bOpen && DoorYawAlpha <= 0.02f)
	{
		bOneWaySealPending = false;
		bOneWaySealed = true;
		ApplyOpenState();   // hide the (now-closed) leaf, raise the flush seal plate
		ForceNetUpdate();
		UE_LOG(CombatForgeLog, Verbose, TEXT("Door piece %u one-way sealed after close swing"), PieceId);
	}
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

	// Playtest 2026-08-06 server-lag pass: this used to walk the FULL world actor list
	// (TActorIterator<ACharacter>) every frame per untriggered trap — O(traps × world actors) at
	// 60 Hz on the server. TrapTrigger already overlaps ECC_Pawn (BuildTrapFloor), so ask the
	// physics scene for its live overlap set instead: cost is proportional to pawns actually
	// standing on the plate (almost always zero). Trip logic below is unchanged, including the
	// feet/center double containment test (the overlap set is the coarse filter, not the truth).
	const FVector Center = TrapTrigger->GetComponentLocation();
	const FVector Ext = TrapTrigger->GetScaledBoxExtent();
	const FBox TriggerBox(Center - Ext, Center + Ext);
	TArray<AActor*> Overlapping;
	TrapTrigger->GetOverlappingActors(Overlapping, ACombatForgeCharacter::StaticClass());
	for (AActor* Actor : Overlapping)
	{
		ACombatForgeCharacter* PFC = Cast<ACombatForgeCharacter>(Actor);
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
