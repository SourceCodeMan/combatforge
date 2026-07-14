// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFTargetDummy.h"

#include "CombatForge.h"
#include "Combat/PFHealthComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float DummyRespawnDelaySec = 1.0f;   // hide 1.0 s then self-reset (contract §3.4)
	// Engine cylinder is r=50, h=100 → (0.8, 0.8, 1.8) ≈ r40 × h180 body.
	const FVector DummyBodyScale(0.8f, 0.8f, 1.8f);
	const FLinearColor DummyGray(0.55f, 0.55f, 0.55f);
}

APFTargetDummy::APFTargetDummy()
{
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;             // existence + bHidden replicate; hits happen server-side
	SetReplicateMovement(false);    // dummies never move

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	SetRootComponent(BodyMesh);
	BodyMesh->SetRelativeScale3D(DummyBodyScale);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BodyMesh->SetCollisionObjectType(ECC_WorldDynamic);
	BodyMesh->SetCollisionResponseToAllChannels(ECR_Block);       // blocks pawns, sightlines...
	BodyMesh->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);   // ...and paintballs
	BodyMesh->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Ignore); // §4.6
	BodyMesh->SetGenerateOverlapEvents(false);
	BodyMesh->SetCastShadow(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		BodyMesh->SetStaticMesh(CylinderFinder.Object);
	}

	Health = CreateDefaultSubobject<UPFHealthComponent>(TEXT("Health"));
	Health->DefaultRoundHP = 1;   // one splat "eliminates" a dummy
}

void APFTargetDummy::BeginPlay()
{
	Super::BeginPlay();

	// Neutral gray body — splats supply the color. MID at runtime (never in the ctor, §5.3).
	if (BodyMID == nullptr && BodyMesh->GetMaterial(0) != nullptr)
	{
		BodyMID = BodyMesh->CreateAndSetMaterialInstanceDynamic(0);
		if (BodyMID != nullptr)
		{
			BodyMID->SetVectorParameterValue(TEXT("Color"), DummyGray);
		}
	}

	if (HasAuthority())
	{
		// Self-resetting: the dummy subscribes to its OWN health component; it never routes
		// to the GameMode and never grants score (T29).
		Health->OnEliminatedEvent.AddUObject(this, &APFTargetDummy::HandleEliminated);
	}
}

void APFTargetDummy::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Health != nullptr)
	{
		Health->OnEliminatedEvent.RemoveAll(this);
	}
	GetWorldTimerManager().ClearTimer(RespawnTimer);
	Super::EndPlay(EndPlayReason);
}

void APFTargetDummy::HandleEliminated(UPFHealthComponent* /*HealthComp*/,
	const FPFPaintHitInfo& /*FinalHit*/)
{
	if (!HasAuthority())
	{
		return;
	}

	UE_LOG(CombatForgeLog, Verbose, TEXT("Target dummy %s splatted; respawn in %.1f s"),
		*GetName(), DummyRespawnDelaySec);

	SetActorHiddenInGame(true);   // bHidden replicates to clients
	GetWorldTimerManager().SetTimer(RespawnTimer, this, &APFTargetDummy::HandleRespawn,
		DummyRespawnDelaySec, false);
}

void APFTargetDummy::HandleRespawn()
{
	if (!HasAuthority())
	{
		return;
	}
	Health->ResetForRound(1);       // restores HP + paintball blocking
	SetActorHiddenInGame(false);
}
