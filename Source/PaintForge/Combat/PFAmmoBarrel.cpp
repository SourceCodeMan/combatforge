// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFAmmoBarrel.h"

#include "PaintForge.h"
#include "Combat/PFWeaponComponent.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgeTypes.h"
#include "Player/PaintForgeCharacter.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPath.h"

APFAmmoBarrel::APFAmmoBarrel()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	bAlwaysRelevant = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	// Solid to BBs (splat) and to pawns (light cover). QueryOnly — no physics sim. The projectile sweeps on
	// PF_ECC_Paintball, so the mesh must Block that channel for a hit/splat to register.
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Mesh->SetCollisionObjectType(ECC_WorldStatic);
	Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	Mesh->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Mesh->SetCastShadow(true);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylFinder.Succeeded())
	{
		Mesh->SetStaticMesh(CylFinder.Object);
		Mesh->SetRelativeScale3D(FVector(1.4f, 1.4f, 1.6f));
	}

	InteractSphere = CreateDefaultSubobject<USphereComponent>(TEXT("InteractSphere"));
	InteractSphere->SetupAttachment(Root);
	InteractSphere->InitSphereRadius(180.f);
	InteractSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	InteractSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	InteractSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	InteractSphere->SetGenerateOverlapEvents(true);

	PromptText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Prompt"));
	PromptText->SetupAttachment(Root);
	PromptText->SetRelativeLocation(FVector(0.f, 0.f, 160.f));
	PromptText->SetHorizontalAlignment(EHTA_Center);
	PromptText->SetVerticalAlignment(EVRTA_TextCenter);
	PromptText->SetWorldSize(28.f);
	PromptText->SetTextRenderColor(FColor(255, 220, 80));
	PromptText->SetText(FText::FromString(TEXT("[F]  REFILL")));
	PromptText->SetVisibility(false);
	PromptText->SetHiddenInGame(true);

	// Always-on floating beacon so the station is obvious from across the arena (not just on overlap).
	SignText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Sign"));
	SignText->SetupAttachment(Root);
	SignText->SetRelativeLocation(FVector(0.f, 0.f, 235.f));
	SignText->SetHorizontalAlignment(EHTA_Center);
	SignText->SetVerticalAlignment(EVRTA_TextCenter);
	SignText->SetWorldSize(52.f);
	SignText->SetTextRenderColor(FColor(255, 210, 40));
	SignText->SetText(FText::FromString(TEXT("AMMO")));
	SignText->SetVisibility(true);
	SignText->SetHiddenInGame(false);
}

void APFAmmoBarrel::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APFAmmoBarrel, bAvailable);
}

void APFAmmoBarrel::BeginPlay()
{
	Super::BeginPlay();
	SoftLoadMesh();

	if (InteractSphere)
	{
		InteractSphere->OnComponentBeginOverlap.AddDynamic(this, &APFAmmoBarrel::OnOverlapBegin);
		InteractSphere->OnComponentEndOverlap.AddDynamic(this, &APFAmmoBarrel::OnOverlapEnd);
	}
	ApplyAvailableVisuals();
	OrientLabels();   // fixed facing once the (replicated) spawn position is known
}

void APFAmmoBarrel::OrientLabels()
{
	// Fixed, non-billboarding facing so the text never flips/mirrors as the player moves. Face the readable
	// side toward the near team's spawn (the field half this barrel sits in): that team can read it, the far
	// side is reversed (acceptable per design). UTextRenderComponent's readable face is -X, so we point +X
	// AWAY from the reader. If it reads backwards in-game, swap the two yaw values (one 180° flip).
	constexpr float HalfFieldX = 3200.f;   // FieldX / 2 (arena is 6400 uu wide)
	const float Yaw = (GetActorLocation().X < HalfFieldX) ? 0.f : 180.f;
	const FRotator Face(0.f, Yaw, 0.f);
	if (SignText)
	{
		SignText->SetWorldRotation(Face);
	}
	if (PromptText)
	{
		PromptText->SetWorldRotation(Face);
	}
}

void APFAmmoBarrel::SoftLoadMesh()
{
	if (!Mesh)
	{
		return;
	}
	const TCHAR* Paths[] = {
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Aba_Storage_Barrel_Metal_Blue_01/SM_Ind_Aba_Storage_Barrel_Metal_Blue_01.SM_Ind_Aba_Storage_Barrel_Metal_Blue_01"),
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Barrel_Plastic_Blue_01/SM_Ind_War_Storage_Barrel_Plastic_Blue_01.SM_Ind_War_Storage_Barrel_Plastic_Blue_01"),
		TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Sto_Barrel_Metal_Rust_03/SM_Ind_Sto_Barrel_Metal_Rust_03.SM_Ind_Sto_Barrel_Metal_Rust_03"),
	};
	for (const TCHAR* Path : Paths)
	{
		if (UStaticMesh* M = Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad()))
		{
			Mesh->SetStaticMesh(M);
			const FBoxSphereBounds B = M->GetBounds();
			const float H = FMath::Max(B.BoxExtent.Z * 2.f, 1.f);
			const float Sc = 140.f / H;
			Mesh->SetRelativeScale3D(FVector(Sc));
			// Sit bottom on ground.
			Mesh->SetRelativeLocation(FVector(
				-B.Origin.X * Sc,
				-B.Origin.Y * Sc,
				-(B.Origin.Z - B.BoxExtent.Z) * Sc));
			return;
		}
	}
}

void APFAmmoBarrel::ServerActivateAt(const FVector& WorldLoc)
{
	if (!HasAuthority())
	{
		return;
	}
	SetActorLocation(WorldLoc);
	bAvailable = true;
	ApplyAvailableVisuals();
	OrientLabels();   // re-face after the authoritative placement
	ForceNetUpdate();
}

void APFAmmoBarrel::ApplyAvailableVisuals()
{
	if (Mesh)
	{
		Mesh->SetVisibility(bAvailable);
		Mesh->SetHiddenInGame(!bAvailable);
	}
	if (InteractSphere)
	{
		InteractSphere->SetCollisionEnabled(
			bAvailable ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
	}
	if (!bAvailable)
	{
		LocalOverlappingPawn.Reset();
	}
	UpdatePromptVisibility();
}

void APFAmmoBarrel::OnRep_Available()
{
	ApplyAvailableVisuals();
}

void APFAmmoBarrel::OnOverlapBegin(UPrimitiveComponent* /*OverlappedComp*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/, const FHitResult& /*SweepResult*/)
{
	APawn* Pawn = Cast<APawn>(OtherActor);
	if (!Pawn || !Pawn->IsLocallyControlled())
	{
		return;
	}
	LocalOverlappingPawn = Pawn;
	UpdatePromptVisibility();
}

void APFAmmoBarrel::OnOverlapEnd(UPrimitiveComponent* /*OverlappedComp*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/)
{
	if (LocalOverlappingPawn.Get() == OtherActor)
	{
		LocalOverlappingPawn.Reset();
		UpdatePromptVisibility();
	}
}

void APFAmmoBarrel::UpdatePromptVisibility()
{
	const bool bShow = bAvailable && LocalOverlappingPawn.IsValid();
	if (PromptText)
	{
		PromptText->SetVisibility(bShow);
		PromptText->SetHiddenInGame(!bShow);
	}
}

void APFAmmoBarrel::LocalRequestInteract()
{
	if (!bAvailable || !LocalOverlappingPawn.IsValid())
	{
		return;
	}
	ServerInteract(LocalOverlappingPawn.Get());
}

void APFAmmoBarrel::ServerInteract_Implementation(APawn* Interactor)
{
	if (!HasAuthority() || !bAvailable || !Interactor)
	{
		return;
	}
	// Only during combat.
	if (const UWorld* World = GetWorld())
	{
		if (const APaintForgeGameState* GS = World->GetGameState<APaintForgeGameState>())
		{
			if (GS->Phase != EPFMatchPhase::Combat)
			{
				return;
			}
		}
	}
	// Must be near the barrel.
	if (FVector::DistSquared(Interactor->GetActorLocation(), GetActorLocation()) > FMath::Square(220.f))
	{
		return;
	}

	APaintForgeCharacter* Char = Cast<APaintForgeCharacter>(Interactor);
	if (!Char)
	{
		return;
	}
	UPFWeaponComponent* Weapon = Char->GetWeapon();
	if (!Weapon || !Weapon->ServerRefillFromPickup())
	{
		return; // already full
	}

	// Permanent resupply station: do NOT consume the barrel. It stays available so any player can top up as
	// often as they need for the whole combat phase. (ServerRefillFromPickup already no-ops when full, so
	// spamming [E] against a full mag does nothing.)
	UE_LOG(PaintForgeLog, Log, TEXT("AmmoBarrel: refilled %s"), *Char->GetName());
}
