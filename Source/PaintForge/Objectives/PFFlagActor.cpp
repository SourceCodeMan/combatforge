// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Objectives/PFFlagActor.h"

#include "PaintForge.h"
#include "Core/PaintForgeGameMode.h"
#include "Core/PaintForgePlayerState.h"
#include "Core/PaintForgeTypes.h"
#include "Objectives/PFObjectiveLayout.h"
#include "Player/PaintForgeCharacter.h"

#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float FlagPickupRadius = 140.f;
}

APFFlagActor::APFFlagActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

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
	ApplyTeamColor();
	// Pole is a neutral gray (BasicShapeMaterial carries a "Color" tint); only the banner is team-colored.
	if (PoleMesh != nullptr)
	{
		if (UMaterialInstanceDynamic* PoleMID = PoleMesh->CreateAndSetMaterialInstanceDynamic(0))
		{
			PoleMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.18f, 0.18f, 0.20f, 1.f));
		}
	}
	if (HasAuthority() && PickupSphere)
	{
		PickupSphere->OnComponentBeginOverlap.AddDynamic(this, &APFFlagActor::OnPickupOverlap);
	}
}

void APFFlagActor::ServerInit(uint8 InOwnerTeam, const FVector& InHomeLocation)
{
	if (!HasAuthority())
	{
		return;
	}
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

void APFFlagActor::ServerGiveTo(APaintForgePlayerState* Carrier)
{
	if (!HasAuthority() || !Carrier || bCarried)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(DropReturnTimer);
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
	UE_LOG(PaintForgeLog, Log, TEXT("Flag team %d auto-returned after drop timeout"), OwnerTeam);
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
	APaintForgePlayerState* PS = CarrierPS.Get();
	if (!PS)
	{
		ServerReturnHome();
		return;
	}
	if (APawn* Pawn = PS->GetPawn())
	{
		SetActorLocation(Pawn->GetActorLocation() + FVector(0.f, 0.f, 80.f));
	}
}

void APFFlagActor::OnPickupOverlap(UPrimitiveComponent* /*OverlappedComponent*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/,
	const FHitResult& /*SweepResult*/)
{
	if (!HasAuthority() || bCarried)
	{
		return;
	}
	const APaintForgeCharacter* Char = Cast<APaintForgeCharacter>(OtherActor);
	if (!Char)
	{
		return;
	}
	APaintForgePlayerState* PS = Char->GetPlayerState<APaintForgePlayerState>();
	if (!PS || PS->TeamId > 1 || !PS->bAliveInRound)
	{
		return;
	}
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
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
	if (FlagMID == nullptr && FlagMesh->GetMaterial(0) != nullptr)
	{
		FlagMID = FlagMesh->CreateAndSetMaterialInstanceDynamic(0);
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
