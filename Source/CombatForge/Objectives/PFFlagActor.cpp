// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Objectives/PFFlagActor.h"

#include "CombatForge.h"
#include "Core/CombatForgeGameMode.h"
#include "Core/CombatForgePlayerState.h"
#include "Core/CombatForgeTypes.h"
#include "Combat/PFHealthComponent.h"
#include "Objectives/PFObjectiveLayout.h"
#include "Player/CombatForgeCharacter.h"

#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	constexpr float FlagPickupRadius = 140.f;
	// "Flag" prefix: unity builds merge anonymous namespaces across the chunk's .cpps, and
	// PFControlPointActor.cpp defines its own PoleBlack — a bare duplicate breaks the merged TU.
	const FLinearColor FlagPoleBlack(0.02f, 0.02f, 0.02f, 1.f);
}

APFFlagActor::APFFlagActor()
{
	PrimaryActorTick.bCanEverTick = true;
	// Tick is ONLY needed to follow a carrier; enabled in ServerGiveTo and disabled again on
	// drop/return, mirroring the control point's pulse pattern. It used to run every frame for
	// the whole match while the flag sat at home doing nothing. (P2-OBJ4)
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates = true;
	SetReplicateMovement(true);
	bAlwaysRelevant = true;

	// A flag from engine primitives (graybox style, no art asset): a thin pole standing on the floor with a
	// team-colored banner hanging off the top. Root is a plain scene node so the pole/banner can sit with the
	// pole base on the actor origin (the floor), which a single centred-pivot mesh couldn't do.
	FlagRoot = CreateDefaultSubobject<USceneComponent>(TEXT("FlagRoot"));
	SetRootComponent(FlagRoot);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));

	// Guaranteed material fallback — hard CDO refs are only reliable for /Engine content (playbook §2);
	// the /Game masters soft-resolve in EnsureFlagMaterials once the world is up.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BaseMatFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BaseMatFinder.Succeeded())
	{
		FallbackBaseMaterial = BaseMatFinder.Object;
	}

	// Pole: thin tall cylinder. Engine cylinder is 100 uu tall with a centred pivot → scale Z 3.4 (~340 uu) and
	// lift half so the base sits on the floor.
	PoleMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PoleMesh"));
	PoleMesh->SetupAttachment(FlagRoot);
	if (CylFinder.Succeeded()) { PoleMesh->SetStaticMesh(CylFinder.Object); }
	PoleMesh->SetRelativeScale3D(FVector(0.10f, 0.10f, 3.4f));
	PoleMesh->SetRelativeLocation(FVector(0.f, 0.f, 170.f));
	PoleMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PoleMesh->SetCastShadow(false);

	// Banner: a flat, wide, tall quad hanging off the top of the pole (+X), team-coloured (ApplyTeamColor targets
	// FlagMesh). Engine cube is 100 uu → ~130 wide x 6 thin x 80 tall, its inner edge on the pole.
	FlagMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FlagMesh"));
	FlagMesh->SetupAttachment(FlagRoot);
	if (CubeFinder.Succeeded()) { FlagMesh->SetStaticMesh(CubeFinder.Object); }
	FlagMesh->SetRelativeScale3D(FVector(1.3f, 0.06f, 0.8f));
	FlagMesh->SetRelativeLocation(FVector(65.f, 0.f, 285.f));
	FlagMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FlagMesh->SetCastShadow(false);

	PickupSphere = CreateDefaultSubobject<USphereComponent>(TEXT("PickupSphere"));
	PickupSphere->SetupAttachment(FlagRoot);
	PickupSphere->SetRelativeLocation(FVector(0.f, 0.f, 100.f));   // centred on the flag body, not the pole base
	PickupSphere->SetSphereRadius(FlagPickupRadius);
	PickupSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PickupSphere->SetCollisionObjectType(ECC_WorldDynamic);
	PickupSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	PickupSphere->SetGenerateOverlapEvents(true);
}

void APFFlagActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APFFlagActor, OwnerTeam);
	DOREPLIFETIME(APFFlagActor, bAtHome);
	DOREPLIFETIME(APFFlagActor, bCarried);
	DOREPLIFETIME(APFFlagActor, HomeLocation);
}

void APFFlagActor::BeginPlay()
{
	Super::BeginPlay();
	EnsureFlagMaterials();
	ApplyTeamColor();
	// Pole: near-black metal MID from the loaded MASTER + explicit SetMaterial — the old
	// CreateAndSetMaterialInstanceDynamic path trusted the default slot (checkerboard when cooked).
	if (PoleMesh != nullptr && PoleMID == nullptr)
	{
		if (UMaterialInterface* PoleMaster = MetalMaterial ? MetalMaterial.Get() : FallbackBaseMaterial.Get())
		{
			PoleMID = UMaterialInstanceDynamic::Create(PoleMaster, this);
			PoleMID->SetVectorParameterValue(TEXT("Color"), FlagPoleBlack);
			PoleMesh->SetMaterial(0, PoleMID);
		}
	}
	if (HasAuthority() && PickupSphere)
	{
		PickupSphere->OnComponentBeginOverlap.AddDynamic(this, &APFFlagActor::OnPickupOverlap);
	}
}

void APFFlagActor::EnsureFlagMaterials()
{
	if (bTriedFlagMaterials)
	{
		return;
	}
	bTriedFlagMaterials = true;
	MarkMaterial = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ArenaMark.M_PF_ArenaMark")).TryLoad());
	if (MarkMaterial == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("[Flag] M_PF_ArenaMark missing (run Scripts/gen_arena_materials.py) — cloth falls back to BasicShapeMaterial"));
	}
	MetalMaterial = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ArenaMetal.M_PF_ArenaMetal")).TryLoad());
	if (MetalMaterial == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("[Flag] M_PF_ArenaMetal missing (run Scripts/gen_arena_materials.py) — pole falls back to BasicShapeMaterial"));
	}
	if (FallbackBaseMaterial == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("[Flag] BasicShapeMaterial fallback missing — flag meshes may render the default checker"));
	}
}

void APFFlagActor::ServerInit(uint8 InOwnerTeam, const FVector& InHomeLocation)
{
	if (!HasAuthority())
	{
		return;
	}
	// A re-init while a drop timer is still armed would let HandleDropReturnTimer fire against the
	// fresh home state. Clear it before writing anything. (P2-OBJ1)
	GetWorldTimerManager().ClearTimer(DropReturnTimer);
	SetActorTickEnabled(false);   // (P2-OBJ4)
	OwnerTeam = InOwnerTeam;
	HomeLocation = InHomeLocation;
	bAtHome = true;
	bCarried = false;
	CarrierPS.Reset();
	SetActorLocation(HomeLocation);
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	ApplyTeamColor();
	ForceNetUpdate();
}

void APFFlagActor::ServerGiveTo(ACombatForgePlayerState* Carrier)
{
	if (!HasAuthority() || !Carrier || bCarried)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(DropReturnTimer);
	SetActorTickEnabled(true);   // follow the carrier (P2-OBJ4)
	CarrierPS = Carrier;
	bCarried = true;
	bAtHome = false;
	SetActorEnableCollision(false);
	ForceNetUpdate();
}

void APFFlagActor::ServerReturnHome()
{
	if (!HasAuthority())
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(DropReturnTimer);
	SetActorTickEnabled(false);   // parked at home, nothing to follow (P2-OBJ4)
	CarrierPS.Reset();
	bCarried = false;
	bAtHome = true;
	SetActorLocation(HomeLocation);
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	ForceNetUpdate();
}

void APFFlagActor::ServerDropAt(const FVector& WorldLoc)
{
	if (!HasAuthority())
	{
		return;
	}
	SetActorTickEnabled(false);   // nothing to follow while it lies on the floor (P2-OBJ4)
	CarrierPS.Reset();
	bCarried = false;
	bAtHome = false;
	SetActorLocation(FVector(WorldLoc.X, WorldLoc.Y, PFObjectiveLayout::FloorZ));
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	// Auto-return so a dropped flag can't soft-lock a kids match forever.
	GetWorldTimerManager().SetTimer(DropReturnTimer, this,
		&APFFlagActor::HandleDropReturnTimer, DropReturnDelaySec, false);
	ForceNetUpdate();
}

void APFFlagActor::HandleDropReturnTimer()
{
	if (!HasAuthority() || bCarried || bAtHome)
	{
		return;
	}
	UE_LOG(CombatForgeLog, Log, TEXT("Flag team %d auto-returned after drop timeout"), OwnerTeam);
	ServerReturnHome();
}

void APFFlagActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(DropReturnTimer);
	Super::EndPlay(EndPlayReason);
}

void APFFlagActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bCarried)
	{
		return;
	}
	// Server owns the carrier pointer and drives location; movement replicates to clients.
	if (!HasAuthority())
	{
		return;
	}
	ACombatForgePlayerState* PS = CarrierPS.Get();
	if (!PS)
	{
		ServerReturnHome();
		return;
	}
	APawn* Pawn = PS->GetPawn();
	if (!Pawn)
	{
		// Carrier's PlayerState is alive but its pawn is gone (eliminated / destroyed before the
		// GameMode's drop ran). Without this the flag hangs in mid-air at the last carried spot and
		// nobody can retake it. Drop it where it is, clearing the carry flag exactly like the
		// GameMode's elimination path does, so the round stays playable. (P2-OBJ2)
		PS->ServerSetFlagCarry(false, 255);
		ServerDropAt(GetActorLocation());
		return;
	}
	SetActorLocation(Pawn->GetActorLocation() + FVector(0.f, 0.f, 80.f));
}

void APFFlagActor::OnPickupOverlap(UPrimitiveComponent* /*OverlappedComponent*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/,
	const FHitResult& /*SweepResult*/)
{
	if (!HasAuthority() || bCarried)
	{
		return;
	}
	const ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(OtherActor);
	if (!Char)
	{
		return;
	}
	ACombatForgePlayerState* PS = Char->GetPlayerState<ACombatForgePlayerState>();
	// bAliveInRound is NOT the elimination truth in Skirmish (it's never cleared on elim — see PFBotController),
	// so an eliminated-but-respawning player would otherwise still return/capture when the flag re-overlaps
	// their corpse (e.g. an enemy drops it on them). Gate on the real elimination flag too.
	const UPFHealthComponent* Health = Char->GetHealth();
	if (!PS || PS->TeamId > 1 || !PS->bAliveInRound || (Health && Health->bEliminated))
	{
		return;
	}
	if (ACombatForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<ACombatForgeGameMode>() : nullptr)
	{
		GM->NotifyFlagTouched(this, PS);
	}
}

void APFFlagActor::OnRep_VisualState()
{
	ApplyTeamColor();
	ApplyVisualState();
}

void APFFlagActor::ApplyTeamColor()
{
	if (!FlagMesh)
	{
		return;
	}
	EnsureFlagMaterials();
	// Cloth MID from the loaded MASTER + explicit SetMaterial (never the default slot / never MID-of-MID).
	// CTF cloth stays TEAM-colored — unlike Domination, a CTF flag is never neutral.
	if (FlagMID == nullptr)
	{
		if (UMaterialInterface* ClothMaster = MarkMaterial ? MarkMaterial.Get() : FallbackBaseMaterial.Get())
		{
			FlagMID = UMaterialInstanceDynamic::Create(ClothMaster, this);
			FlagMesh->SetMaterial(0, FlagMID);
		}
	}
	if (FlagMID)
	{
		FlagMID->SetVectorParameterValue(TEXT("Color"), PFColors::ForTeam(OwnerTeam));
	}
}

void APFFlagActor::ApplyVisualState()
{
	SetActorHiddenInGame(false);
}
