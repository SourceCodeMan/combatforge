// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Core/CombatForgeTypes.h"
#include "PFCharacterMovementComponent.generated.h"

/**
 * CombatForge movement: sprint 830 / walk 600, ADS x0.55, custom CMOVE_Slide
 * with entry boost + friction curve, all client-predicted via compressed flags
 * and FSavedMove_PF (02 D5/R6). Speed changes happen ONLY inside GetMaxSpeed().
 *
 * Flags: FLAG_Custom_0 = WantsToSprint, FLAG_Custom_1 = WantsToADS.
 * bWantsToCrouch rides the stock FLAG_WantsToCrouch; slide entry is derived
 * deterministically from (crouch-wants + sprint flag + speed) on both sides.
 */
UCLASS()
class COMBATFORGE_API UPFCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UPFCharacterMovementComponent();

	/** Custom movement mode values under MOVE_Custom. */
	static constexpr uint8 CMOVE_Slide  = 0;
	static constexpr uint8 CMOVE_Mantle = 1;

	// ---- Input intents (prediction-safe; ride compressed flags, NEVER RPCs — 02 D5) ----
	void SetWantsToSprint(bool bWants);    // FLAG_Custom_0
	void SetWantsToADS(bool bWants);       // FLAG_Custom_1 (speed effect must be in the move stream)
	void SetWantsToMantle(bool bWants);    // FLAG_Custom_2 — 2nd SPACE press while airborne (ledge climb)
	void OnCrouchSlidePressed();           // sets bWantsToCrouch; CMC decides slide vs crouch deterministically
	void OnCrouchSlideReleased();

	// ---- Cross-package queries ----
	bool IsSliding() const;                // MovementMode == MOVE_Custom && CustomMovementMode == CMOVE_Slide
	bool IsMantling() const;               // MovementMode == MOVE_Custom && CustomMovementMode == CMOVE_Mantle
	bool IsSprintingEffective() const;     // sprint flag AND grounded AND input dot forward > 0.5
	bool IsSlideGlidePhase() const;        // glide window (first 0.35 s, extended while descending)
	bool WantsToADS() const;               // FLAG_Custom_1 state: the server-visible ADS intent from the
	                                       // move stream — the character reads it back so the authoritative
	                                       // spread cone matches the owning client's prediction

	FPFOnSlideStateChanged OnSlideStateChanged;    // character binds for FOV kick; fired on both sides
	FPFOnMantleStateChanged OnMantleStateChanged;  // character binds for mantle climb anim

	// ---- Config (ctor defaults per 04) ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float WalkSpeed = 600.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SprintSpeed = 830.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float CrouchSpeed = 300.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float ADSSpeedMult = 0.55f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideMinEnterSpeed = 750.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideBoostSpeed = 1150.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideSteerDegPerSec = 15.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideGlideTime = 0.35f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideGlideFriction = 0.6f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideEndFriction = 8.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideFrictionRampTime = 0.5f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideMinExitSpeed = 320.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideMaxDuration = 1.1f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SlideCooldown = 0.5f;

	// ---- Mantle (double-SPACE ledge climb) ----
	/** Forward reach past the capsule edge that still counts as "at the wall". */
	UPROPERTY(EditDefaultsOnly, Category="PF|Mantle") float MantleReachUU = 55.f;
	/** Ledge-top height band relative to the CURRENT feet position. Min skips normal step-ups; Max ≈ one
	 *  wall level (300) + slack, so a single mantle can clear one build level but never a stacked (600) wall
	 *  or the 1800 perimeter — the arena-containment + fort-integrity invariants stay intact. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Mantle") float MantleMinHeightUU = 60.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Mantle") float MantleMaxHeightUU = 340.f;
	/** Seconds for the climb interp (vertical rise then forward tuck). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Mantle") float MantleTime = 0.35f;

	/**
	 * CoD-style weapon weight. Set from the equipped weapon def in ApplyWeaponLoadout (runs on server +
	 * autonomous proxy via kit rep). MUST only depend on replicated kit state — used in prediction/replay.
	 */
	float CachedWeaponMoveSpeedMult = 1.f;

	// ---- UCharacterMovementComponent overrides ----
	virtual float GetMaxSpeed() const override;                       // the ONLY speed source (02 R6)
	virtual float GetMaxBrakingDeceleration() const override;
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;
	virtual FNetworkPredictionData_Client* GetPredictionData_Client() const override;
	virtual bool CanCrouchInCurrentState() const override;            // stay crouched during CMOVE_Slide
	virtual bool CanAttemptJump() const override;                     // slide-jump keeps horizontal velocity
	virtual void OnTeleported() override;                             // respawn teleport aborts a mantle
	/**
	 * Depenetration guard — the fix for "bots randomly rocket to the roof".
	 * When two capsules overlap, the engine resolves it with a NON-SWEPT teleport along the minimum-translation
	 * axis, up to MaxDepenetrationWithPawn (100uu) / WithGeometry (500uu) EVERY sub-step. Bots get driven into
	 * each other (they score the enemy's own spot as a firing position), so once horizontal escape is blocked the
	 * MTD flips vertical and the pawn is teleported skyward at thousands of uu/s — with Velocity never set, which
	 * is why it reads as an instant launch, not an arc. It only ever hit bots because only bots are commanded to
	 * stand inside another pawn. Falling back past FallLethalHeightUU then kills them -> the observed respawn.
	 */
	virtual FVector GetPenetrationAdjustment(const FHitResult& Hit) const override;

protected:
	virtual void PhysCustom(float deltaTime, int32 Iterations) override;
	virtual void OnMovementUpdated(float DeltaSeconds, const FVector& OldLocation, const FVector& OldVelocity) override;
	virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;

	/** CMOVE_Slide physics: steer 15 deg/s, gravity along slope, friction curve. */
	void PhysSlide(float deltaTime, int32 Iterations);

	/** Boost to SlideBoostSpeed along current velocity direction and enter CMOVE_Slide. */
	void EnterSlide();

	/** CMOVE_Mantle physics: deterministic rise-then-tuck interp from MantleStart to MantleTarget. */
	void PhysMantle(float deltaTime, int32 Iterations);

	/** Capture start/target and enter CMOVE_Mantle. */
	void EnterMantle(const FVector& StandTarget);

	/** Ledge test against STATIC world geometry (deterministic on client + replaying server): a near-vertical
	 *  face ahead, a top within the mantle height band, and capsule clearance above it. OutStandTarget = the
	 *  capsule-center end position (just past the face, fully ABOVE the ledge top). */
	bool DetectMantleLedge(FVector& OutStandTarget) const;

private:
	friend class FSavedMove_PF;

	// Input intents carried by compressed flags (never replicated directly).
	uint8 bWantsToSprintPF : 1;
	uint8 bWantsToADSPF : 1;
	uint8 bWantsToMantlePF : 1;

	// Mantle simulation state. UNLIKE the slide timers this IS carried in FSavedMove_PF (review
	// wf_e923820a): a correction replay would otherwise double-advance MantleElapsed (early snap →
	// sustained correction storm), and a server-forced CMOVE_Mantle mode-apply on a client that never
	// ran EnterMantle would interp toward a zero/stale target. PrepMoveFor restores these per replayed
	// move; PhysMantle's zero/far-target guard degrades the forced-mode case to falling.
	FVector MantleStart = FVector::ZeroVector;
	FVector MantleTarget = FVector::ZeroVector;
	float MantleElapsed = 0.f;

	// Slide simulation state. Advanced only inside the movement simulation
	// (PhysSlide / OnMovementUpdated) so client and server stay in step; not
	// part of saved moves — a server correction mid-slide replays with the
	// current timers, which is within v1 tolerance (entry/exit conditions are
	// flag+speed-derived and deterministic).
	float SlideElapsed = 0.f;
	float SlideRampStartElapsed = 0.f;
	float SlideCooldownRemaining = 0.f;
	uint8 bSlideGlideActive : 1;
};

/** Saved move carrying the PF compressed flags. Must exist under this name (§3.3). */
class COMBATFORGE_API FSavedMove_PF : public FSavedMove_Character
{
public:
	typedef FSavedMove_Character Super;

	uint8 bSavedWantsToSprint : 1;
	uint8 bSavedWantsToADS : 1;
	uint8 bSavedWantsToMantle : 1;   // FLAG_Custom_2

	// Mantle sim state snapshot — restored by PrepMoveFor so correction replays re-run PhysMantle with the
	// state each move ORIGINALLY had (no double-advanced elapsed, no stale target on a forced mode-apply).
	FVector SavedMantleStart = FVector::ZeroVector;
	FVector SavedMantleTarget = FVector::ZeroVector;
	float SavedMantleElapsed = 0.f;

	FSavedMove_PF();

	virtual void Clear() override;
	virtual uint8 GetCompressedFlags() const override;
	virtual bool CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const override;
	virtual void SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, FNetworkPredictionData_Client_Character& ClientData) override;
	virtual void PrepMoveFor(ACharacter* C) override;
};

class COMBATFORGE_API FNetworkPredictionData_Client_PF : public FNetworkPredictionData_Client_Character
{
public:
	typedef FNetworkPredictionData_Client_Character Super;

	FNetworkPredictionData_Client_PF(const UCharacterMovementComponent& ClientMovement);

	virtual FSavedMovePtr AllocateNewMove() override;
};
