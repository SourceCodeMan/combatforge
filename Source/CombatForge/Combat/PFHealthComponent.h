// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/TimerHandle.h"
#include "Core/CombatForgeTypes.h"
#include "PFHealthComponent.generated.h"

class UPrimitiveComponent;

/**
 * 3-HP paint model + elimination broadcast (B2, T1, contract §3.4).
 *
 * Server-only mutation via ApplyPaintHit; HP/bEliminated replicate with OnReps.
 * Does NOT call the GameMode — the GameMode subscribes to OnEliminatedEvent for
 * player pawns; APFTargetDummy subscribes to its own component (T29 decoupling).
 *
 * Corpse rule (04 §2.4): an eliminated body stops generating hit events immediately
 * but keeps blocking paintballs for 0.5 s, then its collision turns off — both the
 * paintball response and pawn blocking (a hidden corpse must not body-block doorways);
 * world collision stays so the hidden pawn keeps standing on the floor.
 */
UCLASS()
class COMBATFORGE_API UPFHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPFHealthComponent();   // SetIsReplicatedByDefault(true)

	UPROPERTY(EditDefaultsOnly, Category="PF|Health") uint8 DefaultRoundHP = 3;   // B2

	UPROPERTY(ReplicatedUsing=OnRep_HP)         uint8 HP = 3;
	UPROPERTY(ReplicatedUsing=OnRep_Eliminated) bool  bEliminated = false;

	// ---- Server API ----
	void ApplyPaintHit(const FPFPaintHitInfo& HitTemplate);
		// fills Damage from region (Body 1 / Mask 2 — T1), decrements HP, fires
		// ClientPaintHitTaken; at 0 → bEliminated, OnEliminatedEvent broadcast, corpse blocks
		// paintballs 0.5 s then collision off (04 §2.4), owner SetEliminatedAppearance(true).
	/** Instant lethal elim (fall from height, etc.). ShooterTeam=255, no shooter credit. */
	void ApplyFallDeath();
	void ResetForRound(uint8 RoundHP);   // server: restore HP (3, or 1 in sudden death), un-eliminate,
	                                     // restore collision/appearance
	EPFBodyRegion ComputeRegion(const FVector& ImpactPoint) const;  // Mask iff Z ≥ capsule-top − 35 uu (T1)

	// ---- Victim-side feedback (owning client) ----
	UFUNCTION(Client, Reliable) void ClientPaintHitTaken(FVector_NetQuantize ShooterLoc,
	                                 uint8 ShooterTeam, uint8 NewHP);
		// broadcasts OnLocalPaintHitTakenEvent → UPFCombatFeedbackWidget (mask splats + direction arc)

	// ---- Subscription points ----
	FPFOnEliminated         OnEliminatedEvent;         // SERVER-side broadcast (GameMode, dummy)
	FPFOnHPChanged          OnHPChangedEvent;          // both sides via OnRep + server set
	FPFOnLocalPaintHitTaken OnLocalPaintHitTakenEvent; // owning client only

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_HP();
	UFUNCTION() void OnRep_Eliminated();  // hides pawn locally, no ragdoll (02 §1.3)

private:
	// 04 §2.4: 0.5 s after elimination the corpse's collision turns off — paintball response
	// AND pawn blocking (never an invisible body-blocker); world responses untouched. Runs on
	// every machine (server via ApplyPaintHit's timer, clients via OnRep_Eliminated) so
	// client-predicted movement agrees with authority about walking through corpses.
	void DisableCorpseCollision();
	void RestoreCorpseCollision();
	// Applies/clears the eliminated look on a character owner (no-op for dummies).
	void ApplyEliminatedAppearance(bool bNewEliminated);

	FTimerHandle CorpseCollisionTimer;
	// Components whose PF_ECC_Paintball / ECC_Pawn response we flipped to Ignore, for exact restore.
	TArray<TWeakObjectPtr<UPrimitiveComponent>> PaintballBlockersDisabled;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> PawnBlockersDisabled;
};
