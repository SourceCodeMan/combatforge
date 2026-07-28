// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/TimerHandle.h"
#include "Core/CombatForgeTypes.h"
#include "PFHealthComponent.generated.h"

class UPrimitiveComponent;

/**
 * Locational hit model + elimination broadcast (Tom 2026-07-15; supersedes the 3-HP model).
 *
 * A combatant is OUT at 3 head hits, 5 chest hits, 8 limb hits, or 10 total hits —
 * whichever threshold is crossed first. Sudden-death/showdown (ResetForRound(1)) and
 * warm-up dummies use one-hit mode instead. Server-only mutation via ApplyPaintHit;
 * the four counters + bEliminated replicate with OnReps.
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

	UPROPERTY(EditDefaultsOnly, Category="PF|Health") uint8 DefaultRoundHP = 3;   // legacy knob: <=1 → one-hit mode

	/** Owners that VANISH on elimination (warm-up dummies) skip the 0.5 s corpse-block window —
	 *  otherwise warm-up shots hit invisible air for half a second after the target disappears.
	 *  Characters leave this false: their corpse is still visible for that window. (P2-CB10) */
	UPROPERTY(EditDefaultsOnly, Category="PF|Health") bool bSkipCorpseBlock = false;

	// Out thresholds (Tom 2026-07-15): whichever is crossed first eliminates.
	UPROPERTY(EditDefaultsOnly, Category="PF|Health") uint8 HeadOut  = 3;
	UPROPERTY(EditDefaultsOnly, Category="PF|Health") uint8 ChestOut = 5;
	UPROPERTY(EditDefaultsOnly, Category="PF|Health") uint8 LimbOut  = 8;
	UPROPERTY(EditDefaultsOnly, Category="PF|Health") uint8 TotalOut = 10;

	UPROPERTY(ReplicatedUsing=OnRep_Hits)       uint8 HeadHits  = 0;
	UPROPERTY(ReplicatedUsing=OnRep_Hits)       uint8 ChestHits = 0;
	UPROPERTY(ReplicatedUsing=OnRep_Hits)       uint8 LimbHits  = 0;
	UPROPERTY(ReplicatedUsing=OnRep_Hits)       uint8 TotalHits = 0;
	UPROPERTY(Replicated)                       bool  bOneHitMode = false;   // showdown / warm-up dummies
	UPROPERTY(ReplicatedUsing=OnRep_Eliminated) bool  bEliminated = false;

	// ---- Server API ----
	// bForceEliminate = this hit eliminates outright regardless of the region thresholds (a melee tag —
	// "you're out"); the shooter in HitTemplate still gets the elim-feed + score credit.
	void ApplyPaintHit(const FPFPaintHitInfo& HitTemplate, bool bForceEliminate = false);
		// resolves the body region (bone name → nearest-bone scan → Z-band), bumps the region +
		// total counters, fires ClientPaintHitTaken; when a threshold crosses → bEliminated,
		// OnEliminatedEvent broadcast, corpse blocks paintballs 0.5 s then collision off (04 §2.4).
	/** Instant lethal elim (fall from height, etc.). ShooterTeam=255, no shooter credit. */
	void ApplyFallDeath();
	void ResetForRound(uint8 RoundHP);   // server: zero counters, un-eliminate, restore collision/
	                                     // appearance; RoundHP<=1 → one-hit mode (showdown)
	EPFBodyRegion ComputeRegion(const FVector& ImpactPoint) const;  // legacy Z-band: Head iff Z ≥ capsule-top − 35 uu
	/** Bone name (if any) wins; else nearest-bone scan on the owner's skeletal mesh; else Z-band. */
	EPFBodyRegion ResolveHitRegion(const FName& HitBone, const FVector& ImpactPoint) const;
	/** min per-region remaining — the legacy "HP" number for feedback widgets/stingers (0 = out). */
	uint8 NearestRemaining() const;

	// ---- Victim-side feedback (owning client) ----
	UFUNCTION(Client, Reliable) void ClientPaintHitTaken(FVector_NetQuantize ShooterLoc,
	                                 uint8 ShooterTeam, uint8 NewHP);
		// broadcasts OnLocalPaintHitTakenEvent → UPFCombatFeedbackWidget (mask splats + direction arc)

	// ---- Subscription points ----
	FPFOnEliminated         OnEliminatedEvent;         // SERVER-side broadcast (GameMode, dummy)
	FPFOnHPChanged          OnHPChangedEvent;          // both sides via OnRep + server set (NearestRemaining)
	FPFOnHitsChanged        OnHitsChangedEvent;        // both sides: per-region + total counters (HUD)
	FPFOnLocalPaintHitTaken OnLocalPaintHitTakenEvent; // owning client only

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_Hits();        // shared OnRep for all four counters
	UFUNCTION() void OnRep_Eliminated();  // hides pawn locally, no ragdoll (02 §1.3)

private:
	// 04 §2.4: 0.5 s after elimination the corpse's collision turns off — paintball response
	// AND pawn blocking (never an invisible body-blocker); world responses untouched. Runs on
	// every machine (server via ApplyPaintHit's timer, clients via OnRep_Eliminated) so
	// client-predicted movement agrees with authority about walking through corpses.
	void DisableCorpseCollision();
	void RestoreCorpseCollision();
	/** Arms the 0.5 s corpse-block window, or drops collision now when the owner hides instantly. */
	void ArmCorpseCollisionOff();
	// Applies/clears the eliminated look on a character owner (no-op for dummies).
	void ApplyEliminatedAppearance(bool bNewEliminated);

	FTimerHandle CorpseCollisionTimer;
	// Components whose PF_ECC_Paintball / ECC_Pawn response we flipped to Ignore, for exact restore.
	TArray<TWeakObjectPtr<UPrimitiveComponent>> PaintballBlockersDisabled;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> PawnBlockersDisabled;
};
