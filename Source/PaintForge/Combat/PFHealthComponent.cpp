// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFHealthComponent.h"

#include "PaintForge.h"
#include "Combat/PFCombatAudio.h"
#include "Player/PaintForgeCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Core/PaintForgePlayerState.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "Engine/World.h"

namespace
{
	constexpr float MaskBandFromCapsuleTopUU = 35.f;   // T1
	constexpr float CorpseBlockSeconds = 0.5f;         // 04 §2.4
}

UPFHealthComponent::UPFHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UPFHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetOwnerRole() == ROLE_Authority)
	{
		HP = DefaultRoundHP;
		OnHPChangedEvent.Broadcast(HP);   // host UI (R9)
	}
}

void UPFHealthComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CorpseCollisionTimer);
	}
	Super::EndPlay(EndPlayReason);
}

void UPFHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UPFHealthComponent, HP);
	DOREPLIFETIME(UPFHealthComponent, bEliminated);
}

void UPFHealthComponent::ApplyPaintHit(const FPFPaintHitInfo& HitTemplate)
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		UE_LOG(PaintForgeLog, Error, TEXT("ApplyPaintHit called without authority on %s"),
			*GetNameSafe(GetOwner()));
		return;
	}
	// Corpse rule (04 §2.4): eliminated bodies still block paintballs briefly but never
	// generate hit events.
	if (bEliminated || HP == 0)
	{
		return;
	}

	FPFPaintHitInfo Hit = HitTemplate;
	Hit.Region = ComputeRegion(FVector(Hit.ImpactPoint));
	Hit.Damage = (Hit.Region == EPFBodyRegion::Mask) ? 2 : 1;   // B2 / T1

	HP = (HP > Hit.Damage) ? static_cast<uint8>(HP - Hit.Damage) : 0;
	OnHPChangedEvent.Broadcast(HP);   // manual host broadcast (§5.9)

	UE_LOG(PaintForgeLog, Verbose, TEXT("PaintHit on %s: region=%s dmg=%u hp=%u"),
		*GetNameSafe(GetOwner()),
		Hit.Region == EPFBodyRegion::Mask ? TEXT("Mask") : TEXT("Body"),
		static_cast<uint32>(Hit.Damage), static_cast<uint32>(HP));

	// Victim-side feedback (owning client): direction arc + mask splats need the shooter's
	// world position; fall back to the impact point if the shooter pawn is gone.
	FVector ShooterLoc = FVector(Hit.ImpactPoint);
	if (Hit.ShooterPS != nullptr)
	{
		if (const APawn* ShooterPawn = Hit.ShooterPS->GetPawn())
		{
			ShooterLoc = ShooterPawn->GetActorLocation();
		}
	}
	// Route victim feedback only where it can land: a locally-controlled pawn (listen host —
	// the client RPC runs in-place) or a pawn with an owning client connection. Server-owned
	// victims (APFTargetDummy) have neither; sending would only log a
	// "no owning connection" net warning per warm-up hit.
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (OwnerPawn != nullptr &&
		(OwnerPawn->IsLocallyControlled() || OwnerPawn->GetNetConnection() != nullptr))
	{
		ClientPaintHitTaken(ShooterLoc, Hit.ShooterTeam, HP);
	}

	if (HP == 0)
	{
		bEliminated = true;
		ApplyEliminatedAppearance(true);   // server/host visual; clients via OnRep_Eliminated

		// Corpse blocks paintballs 0.5 s, then collision turns off — paintball response and
		// pawn blocking both (04 §2.4). Clients mirror this via OnRep_Eliminated.
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(CorpseCollisionTimer, this,
				&UPFHealthComponent::DisableCorpseCollision, CorpseBlockSeconds, false);
		}

		// GameMode (player pawns) / APFTargetDummy (itself) subscribe here — this component
		// never calls the GameMode directly (T29 decoupling).
		OnEliminatedEvent.Broadcast(this, Hit);
	}
}

void UPFHealthComponent::ApplyFallDeath()
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	if (bEliminated || HP == 0)
	{
		return;
	}

	HP = 0;
	OnHPChangedEvent.Broadcast(HP);

	bEliminated = true;
	ApplyEliminatedAppearance(true);
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(CorpseCollisionTimer, this,
			&UPFHealthComponent::DisableCorpseCollision, CorpseBlockSeconds, false);
	}

	FPFPaintHitInfo Hit;
	Hit.ShooterPS = nullptr;
	Hit.ShooterTeam = 255;   // sentinel: fall / environment (GameMode treats as instant respawn)
	if (const AActor* Owner = GetOwner())
	{
		Hit.ImpactPoint = Owner->GetActorLocation();
	}
	Hit.ImpactNormal = FVector::UpVector;
	Hit.Region = EPFBodyRegion::Body;
	Hit.Damage = 3;
	if (const UWorld* World = GetWorld())
	{
		Hit.ServerTime = World->GetTimeSeconds();
	}

	UE_LOG(PaintForgeLog, Log, TEXT("FallDeath on %s"), *GetNameSafe(GetOwner()));
	OnEliminatedEvent.Broadcast(this, Hit);
}

void UPFHealthComponent::ResetForRound(uint8 RoundHP)
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CorpseCollisionTimer);
	}

	HP = RoundHP;
	bEliminated = false;
	RestoreCorpseCollision();
	ApplyEliminatedAppearance(false);
	OnHPChangedEvent.Broadcast(HP);   // manual host broadcast (§5.9)
}

EPFBodyRegion UPFHealthComponent::ComputeRegion(const FVector& ImpactPoint) const
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return EPFBodyRegion::Body;
	}

	// Mask iff impact Z ≥ capsule-top − 35 uu (T1; works standing and crouched because the
	// capsule top follows the crouch interp). Non-character owners (target dummies) use
	// their bounds top for the same 35 uu band.
	float TopZ = 0.f;
	if (const ACharacter* OwnerChar = Cast<ACharacter>(Owner))
	{
		const UCapsuleComponent* Capsule = OwnerChar->GetCapsuleComponent();
		TopZ = Capsule->GetComponentLocation().Z + Capsule->GetScaledCapsuleHalfHeight();
	}
	else
	{
		TopZ = Owner->GetComponentsBoundingBox().Max.Z;
	}

	return (ImpactPoint.Z >= TopZ - MaskBandFromCapsuleTopUU) ? EPFBodyRegion::Mask : EPFBodyRegion::Body;
}

void UPFHealthComponent::ClientPaintHitTaken_Implementation(FVector_NetQuantize ShooterLoc,
	uint8 ShooterTeam, uint8 NewHP)
{
	OnLocalPaintHitTakenEvent.Broadcast(FVector(ShooterLoc), ShooterTeam, NewHP);

	if (const AActor* Owner = GetOwner())
	{
		if (UPFCombatAudio* Audio = Owner->FindComponentByClass<UPFCombatAudio>())
		{
			Audio->PlaySplatIncoming();
		}
	}
}

void UPFHealthComponent::OnRep_HP()
{
	OnHPChangedEvent.Broadcast(HP);
}

void UPFHealthComponent::OnRep_Eliminated()
{
	// Single-shot tolerant (§5.10): applying the final value directly is always correct.
	ApplyEliminatedAppearance(bEliminated);

	// Mirror the server's corpse collision-off locally: client-predicted movement must agree
	// with authority that a corpse stops blocking pawns, or living players rubber-band on an
	// invisible capsule that no longer exists server-side.
	if (UWorld* World = GetWorld())
	{
		if (bEliminated)
		{
			World->GetTimerManager().SetTimer(CorpseCollisionTimer, this,
				&UPFHealthComponent::DisableCorpseCollision, CorpseBlockSeconds, false);
		}
		else
		{
			World->GetTimerManager().ClearTimer(CorpseCollisionTimer);
			RestoreCorpseCollision();
		}
	}
}

void UPFHealthComponent::DisableCorpseCollision()
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return;
	}

	PaintballBlockersDisabled.Reset();
	PawnBlockersDisabled.Reset();

	TInlineComponentArray<UPrimitiveComponent*> Prims(Owner);
	for (UPrimitiveComponent* Prim : Prims)
	{
		if (Prim == nullptr)
		{
			continue;
		}
		if (Prim->GetCollisionResponseToChannel(PF_ECC_Paintball) == ECR_Block)
		{
			Prim->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Ignore);
			PaintballBlockersDisabled.Add(Prim);
		}
		// "Then collision off" (04 §2.4): a hidden corpse must not remain a pawn-blocking
		// invisible collider (doorway body-blocking). Only the pawn response is dropped —
		// world responses stay so the hidden pawn does not fall out of the level.
		if (Prim->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block)
		{
			Prim->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			PawnBlockersDisabled.Add(Prim);
		}
	}
}

void UPFHealthComponent::RestoreCorpseCollision()
{
	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrim : PaintballBlockersDisabled)
	{
		if (UPrimitiveComponent* Prim = WeakPrim.Get())
		{
			Prim->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
		}
	}
	PaintballBlockersDisabled.Reset();

	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrim : PawnBlockersDisabled)
	{
		if (UPrimitiveComponent* Prim = WeakPrim.Get())
		{
			Prim->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		}
	}
	PawnBlockersDisabled.Reset();
}

void UPFHealthComponent::ApplyEliminatedAppearance(bool bNewEliminated)
{
	if (APaintForgeCharacter* OwnerChar = Cast<APaintForgeCharacter>(GetOwner()))
	{
		OwnerChar->SetEliminatedAppearance(bNewEliminated);
	}
	// Target dummies drive their own hide/show from OnEliminatedEvent.
}
