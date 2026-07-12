// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Objectives/PFFlagActor.h"

#include "PaintForge.h"
#include "Core/PaintForgeGameMode.h"
#include "Core/PaintForgePlayerState.h"
#include "Core/PaintForgeTypes.h"
#include "Objectives/PFObjectiveLayout.h"
#include "Player/PaintForgeCharacter.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float FlagPickupRadius = 120.f;
	const FVector FlagMeshScale(0.6f, 0.6f, 1.4f);
}

APFFlagActor::APFFlagActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;
	SetReplicateMovement(true);
	bAlwaysRelevant = true;

	FlagMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FlagMesh"));
	SetRootComponent(FlagMesh);
	FlagMesh->SetRelativeScale3D(FlagMeshScale);
	FlagMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FlagMesh->SetCastShadow(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(
		TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (ConeFinder.Succeeded())
	{
		FlagMesh->SetStaticMesh(ConeFinder.Object);
	}

	PickupSphere = CreateDefaultSubobject<USphereComponent>(TEXT("PickupSphere"));
	PickupSphere->SetupAttachment(FlagMesh);
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
