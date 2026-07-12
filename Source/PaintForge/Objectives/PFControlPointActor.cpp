// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Objectives/PFControlPointActor.h"

#include "PaintForge.h"
#include "Core/PaintForgePlayerState.h"
#include "Core/PaintForgeTypes.h"
#include "Player/PaintForgeCharacter.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float CaptureRadiusUU = 350.f;   // ~0.9 cell
	const FVector PadScale(3.5f, 3.5f, 0.15f); // flat disc on the floor
	const FLinearColor NeutralGray(0.45f, 0.45f, 0.5f);
}

APFControlPointActor::APFControlPointActor()
{
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	SetReplicateMovement(false);
	bAlwaysRelevant = true;

	PadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PadMesh"));
	SetRootComponent(PadMesh);
	PadMesh->SetRelativeScale3D(PadScale);
	PadMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PadMesh->SetCastShadow(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		PadMesh->SetStaticMesh(CylinderFinder.Object);
	}

	CaptureSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CaptureSphere"));
	CaptureSphere->SetupAttachment(PadMesh);
	CaptureSphere->SetSphereRadius(CaptureRadiusUU);
	CaptureSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CaptureSphere->SetCollisionObjectType(ECC_WorldDynamic);
	CaptureSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	CaptureSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	CaptureSphere->SetGenerateOverlapEvents(true);
}

void APFControlPointActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APFControlPointActor, PointIndex);
	DOREPLIFETIME(APFControlPointActor, ControllingTeam);
	DOREPLIFETIME(APFControlPointActor, bActive);
}

void APFControlPointActor::BeginPlay()
{
	Super::BeginPlay();
	ApplyVisualState();
}

void APFControlPointActor::ServerInit(int32 InPointIndex, const FVector& WorldLoc, bool bInitiallyActive)
{
	if (!HasAuthority())
	{
		return;
	}
	PointIndex = InPointIndex;
	ControllingTeam = 255;
	bActive = bInitiallyActive;
	SetActorLocation(WorldLoc);
	SetActorHiddenInGame(!bActive);
	SetActorEnableCollision(bActive);
	ApplyVisualState();
	ForceNetUpdate();
}

void APFControlPointActor::ServerSetActive(bool bNewActive)
{
	if (!HasAuthority() || bActive == bNewActive)
	{
		return;
	}
	bActive = bNewActive;
	if (!bActive)
	{
		ControllingTeam = 255;
	}
	SetActorHiddenInGame(!bActive);
	SetActorEnableCollision(bActive);
	ApplyVisualState();
	ForceNetUpdate();
}

void APFControlPointActor::ServerSetControllingTeam(uint8 Team)
{
	if (!HasAuthority())
	{
		return;
	}
	const uint8 Clamped = (Team <= 1) ? Team : 255;
	if (ControllingTeam == Clamped)
	{
		return;
	}
	ControllingTeam = Clamped;
	ApplyVisualState();
	ForceNetUpdate();
}

uint8 APFControlPointActor::ServerQueryOccupancy(int32& OutA, int32& OutB) const
{
	OutA = 0;
	OutB = 0;
	if (!HasAuthority() || !bActive || !CaptureSphere)
	{
		return 255;
	}

	TArray<AActor*> Overlapping;
	CaptureSphere->GetOverlappingActors(Overlapping, APaintForgeCharacter::StaticClass());
	for (AActor* Actor : Overlapping)
	{
		const APaintForgeCharacter* Char = Cast<APaintForgeCharacter>(Actor);
		if (!Char)
		{
			continue;
		}
		const APaintForgePlayerState* PS = Char->GetPlayerState<APaintForgePlayerState>();
		if (!PS || !PS->bAliveInRound)
		{
			continue;
		}
		if (PS->TeamId == 0) { ++OutA; }
		else if (PS->TeamId == 1) { ++OutB; }
	}

	if (OutA > 0 && OutB == 0) { return 0; }
	if (OutB > 0 && OutA == 0) { return 1; }
	return 255; // empty or contested
}

void APFControlPointActor::OnRep_VisualState()
{
	ApplyVisualState();
}

void APFControlPointActor::ApplyVisualState()
{
	if (PadMesh)
	{
		PadMesh->SetVisibility(bActive);
	}
	SetActorHiddenInGame(!bActive);

	if (!PadMesh)
	{
		return;
	}
	if (PadMID == nullptr && PadMesh->GetMaterial(0) != nullptr)
	{
		PadMID = PadMesh->CreateAndSetMaterialInstanceDynamic(0);
	}
	if (PadMID)
	{
		const FLinearColor Color = (ControllingTeam <= 1)
			? PFColors::ForTeam(ControllingTeam)
			: NeutralGray;
		PadMID->SetVectorParameterValue(TEXT("Color"), Color);
	}
}
