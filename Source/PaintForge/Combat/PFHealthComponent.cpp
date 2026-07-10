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
	ClientPaintHitTaken(ShooterLoc, Hit.ShooterTeam, HP);

	if (HP == 0)
	{
		bEliminated = true;
		ApplyEliminatedAppearance(true);   // server/host visual; clients via OnRep_Eliminated

		// Corpse blocks paintballs 0.5 s, then the paintball response turns off (04 §2.4).
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(CorpseCollisionTimer, this,
				&UPFHealthComponent::DisablePaintballBlocking, CorpseBlockSeconds, false);
		}

		// GameMode (player pawns) / APFTargetDummy (itself) subscribe here — this component
		// never calls the GameMode directly (T29 decoupling).
		OnEliminatedEvent.Broadcast(this, Hit);
	}
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
	RestorePaintballBlocking();
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
}

void UPFHealthComponent::DisablePaintballBlocking()
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return;
	}

	PaintballBlockersDisabled.Reset();

	TInlineComponentArray<UPrimitiveComponent*> Prims(Owner);
	for (UPrimitiveComponent* Prim : Prims)
	{
		if (Prim != nullptr &&
			Prim->GetCollisionResponseToChannel(PF_ECC_Paintball) == ECR_Block)
		{
			Prim->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Ignore);
			PaintballBlockersDisabled.Add(Prim);
		}
	}
}

void UPFHealthComponent::RestorePaintballBlocking()
{
	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrim : PaintballBlockersDisabled)
	{
		if (UPrimitiveComponent* Prim = WeakPrim.Get())
		{
			Prim->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);
		}
	}
	PaintballBlockersDisabled.Reset();
}

void UPFHealthComponent::ApplyEliminatedAppearance(bool bNewEliminated)
{
	if (APaintForgeCharacter* OwnerChar = Cast<APaintForgeCharacter>(GetOwner()))
	{
		OwnerChar->SetEliminatedAppearance(bNewEliminated);
	}
	// Target dummies drive their own hide/show from OnEliminatedEvent.
}
