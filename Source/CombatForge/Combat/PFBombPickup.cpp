// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFBombPickup.h"

#include "CombatForge.h"
#include "Combat/PFSoftMeshFit.h"
#include "Core/CombatForgeGameMode.h"
#include "Core/CombatForgeGameState.h"
#include "Player/CombatForgeCharacter.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

APFBombPickup::APFBombPickup()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(true);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylFinder.Succeeded())
	{
		Mesh->SetStaticMesh(CylFinder.Object);
		Mesh->SetRelativeScale3D(FVector(0.4f, 0.4f, 0.25f));
	}

	InteractSphere = CreateDefaultSubobject<USphereComponent>(TEXT("Interact"));
	InteractSphere->SetupAttachment(Root);
	InteractSphere->InitSphereRadius(InteractRangeUU);
	InteractSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	InteractSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	InteractSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	InteractSphere->SetGenerateOverlapEvents(true);

	SignText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Sign"));
	SignText->SetupAttachment(Root);
	SignText->SetRelativeLocation(FVector(0.f, 0.f, 70.f));
	SignText->SetHorizontalAlignment(EHTA_Center);
	SignText->SetVerticalAlignment(EVRTA_TextCenter);
	SignText->SetWorldSize(48.f);
	SignText->SetTextRenderColor(FColor(255, 80, 40));
	SignText->SetText(FText::FromString(TEXT("BOMB  ·  F")));
}

void APFBombPickup::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APFBombPickup, bAvailable);
}

void APFBombPickup::BeginPlay()
{
	Super::BeginPlay();
	SoftLoadMesh();
	ApplyAvailableVisuals();
}

void APFBombPickup::SoftLoadMesh()
{
	if (!Mesh)
	{
		return;
	}
	const TCHAR* Paths[] = {
		TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Explosives/01/SM_Modern_Weapons_Explosive_01.SM_Modern_Weapons_Explosive_01"),
		TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Explosives/02/SM_Modern_Weapons_Explosive_02.SM_Modern_Weapons_Explosive_02"),
		TEXT("/Game/MarketplaceBlockout/Modern/Weapons/Assets/Explosives/03/SM_Modern_Weapons_Explosive_03.SM_Modern_Weapons_Explosive_03"),
	};
	if (UStaticMesh* M = PFTryLoadStaticMesh(Paths))
	{
		Mesh->SetStaticMesh(M);
		PFFitMeshCenterToSize(M, Mesh, 80.f);
	}
}

void APFBombPickup::ServerActivateAt(const FVector& WorldLoc)
{
	if (!HasAuthority())
	{
		return;
	}
	SetActorLocation(WorldLoc + FVector(0.f, 0.f, HoverHeightUU));
	bAvailable = true;
	ApplyAvailableVisuals();
	ForceNetUpdate();
}

void APFBombPickup::AuthorityInteract(APawn* Interactor)
{
	if (!HasAuthority() || !bAvailable || !Interactor)
	{
		return;
	}
	ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(Interactor);
	if (!Char || Char->IsCarryingBomb())
	{
		return;   // already holding a charge — leave the pickup for someone else
	}
	const ACombatForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
	if (!GS || GS->Phase != EPFMatchPhase::Combat || !GS->IsFireAllowed())
	{
		return;
	}
	if (FVector::DistSquared(Interactor->GetActorLocation(), GetActorLocation())
		> FMath::Square(InteractRangeUU * 1.25f))
	{
		return;
	}

	Char->GrantBombCharge();
	bAvailable = false;
	ApplyAvailableVisuals();
	ForceNetUpdate();

	// Tell GameMode to schedule the next floating charge (5 s).
	if (ACombatForgeGameMode* GM = GetWorld()->GetAuthGameMode<ACombatForgeGameMode>())
	{
		GM->NotifyBombPickupTaken();
	}
	UE_LOG(CombatForgeLog, Log, TEXT("BombPickup: claimed by %s"), *GetNameSafe(Interactor));
}

void APFBombPickup::OnRep_Available()
{
	ApplyAvailableVisuals();
}

void APFBombPickup::ApplyAvailableVisuals()
{
	const bool bShow = bAvailable;
	if (Mesh)
	{
		Mesh->SetVisibility(bShow);
	}
	if (SignText)
	{
		SignText->SetVisibility(bShow);
		SignText->SetText(FText::FromString(bShow ? TEXT("BOMB  ·  F") : TEXT("")));
	}
	if (InteractSphere)
	{
		InteractSphere->SetCollisionEnabled(bShow ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
	}
	SetActorTickEnabled(bShow);
}

void APFBombPickup::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bAvailable)
	{
		return;
	}
	// Slow spin so the mid-field charge reads as a live pickup, not a static prop.
	SpinYaw += DeltaSeconds * 60.f;
	if (Mesh)
	{
		Mesh->SetRelativeRotation(FRotator(0.f, SpinYaw, 0.f));
	}
	UpdateLabelFacing();
}

void APFBombPickup::UpdateLabelFacing()
{
	if (!SignText || !bAvailable)
	{
		return;
	}
	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	FVector CamLoc; FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	const FVector ToCam = CamLoc - SignText->GetComponentLocation();
	SignText->SetWorldRotation(ToCam.GetSafeNormal2D().Rotation());
}
