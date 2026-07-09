// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Engine/TimerHandle.h"
#include "PaintForgeCharacter.generated.h"

class UCameraComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UPFBuildComponent;
class UPFCharacterMovementComponent;
class UPFCombatAudio;
class UPFHealthComponent;
class UPFWeaponComponent;
struct FInputActionValue;

/**
 * The one pawn for both Build and Combat phases (no pawn swap — 02 D1).
 * First-person camera at capsule-top - 10 uu, graybox body from scaled engine
 * cubes (team-color MID, hidden to owner), single FOV arbiter composing
 * base 105 / ADS 70 / sprint +6 / slide +9 (04 §1.3). Zero head bob.
 */
UCLASS()
class PAINTFORGE_API APaintForgeCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	// ObjectInitializer ctor is mandatory: swaps in UPFCharacterMovementComponent (§5.4).
	APaintForgeCharacter(const FObjectInitializer& ObjectInitializer);

	// ---- Component accessors (cross-package; never null after construction) ----
	UPFCharacterMovementComponent* GetPFMovement() const;
	UPFWeaponComponent*  GetWeapon() const;
	UPFBuildComponent*   GetBuild()  const;
	UPFHealthComponent*  GetHealth() const;
	UPFCombatAudio*      GetCombatAudio() const;
	UCameraComponent*    GetFirstPersonCamera() const;

	// ---- ADS (owned here; CMC + weapon read through these) ----
	void  SetADS(bool bWantsADS);          // input entry; respects slide rule (queue during slide — 04 §1.2)
	bool  IsADS() const;                   // target state
	float GetADSAlpha() const;             // 0..1 transition alpha (0.18 in / 0.14 out) — weapon spread lerp input

	// ---- Team + elimination cosmetics (pkg-weapons calls these) ----
	void  SetTeamColor(uint8 TeamId);      // MID tint on the graybox mesh
	void  SetEliminatedAppearance(bool bEliminated); // hide mesh; collision handled by health component
	FVector GetMuzzleLocation(bool bCosmetic) const; // 04 §2.2: server = capsule offset; cosmetic = camera+20fwd

	// ---- AActor / ACharacter ----
	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode) override;

protected:
	// ---- Enhanced Input handlers ----
	void OnMoveInput(const FInputActionValue& Value);
	void OnLookInput(const FInputActionValue& Value);
	void OnJumpPressed();
	void OnJumpReleased();
	void OnSprintPressed();
	void OnSprintReleased();
	void OnCrouchSlidePressed();
	void OnCrouchSlideReleased();
	void OnFirePressed();
	void OnFireReleased();
	void OnADSPressed();
	void OnADSReleased();
	void OnReloadPressed();

	/** Sprint-out raise timer elapsed — release the buffered fire (04 §1.1). */
	void OnSprintOutFinished();

	/** Deferred StopJumping after a buffered landing jump fired. */
	void ClearBufferedJump();

	/** Recomputes the CMC sprint/ADS intents from held keys + slide state. */
	void UpdateMovementIntents();

	/** Slide state delegate from the CMC (both sides; we only use it locally). */
	void HandleSlideStateChanged(bool bSliding);

	/** Single compose point for every FOV effect (04 §1.3). */
	void UpdateTargetFOV(float DeltaSeconds);

private:
	// ---- Components ----
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UCameraComponent> FirstPersonCamera;
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UStaticMeshComponent> BodyMesh;
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UStaticMeshComponent> HeadMesh;
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UPFWeaponComponent> WeaponComponent;
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UPFBuildComponent> BuildComponent;
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UPFHealthComponent> HealthComponent;
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UPFCombatAudio> CombatAudioComponent;
	UPROPERTY(Transient) TObjectPtr<UPFCharacterMovementComponent> PFMovement;

	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> BodyMID;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> HeadMID;

	// ---- Config (04 §1) ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float BaseFOV = 105.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float ADSFOV = 70.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float ADSInTime = 0.18f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float ADSOutTime = 0.14f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float SprintFOVKick = 6.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float SprintKickTime = 0.15f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float SlideFOVKick = 9.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float CameraEyeOffsetFromCapsuleTop = 10.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float CrouchCameraInterpSpeed = 150.f; // 30 uu over 0.2 s
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float LandingDipMinFallUU = 300.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float SprintOutTime = 0.18f; // sprint -> first shot (04 §1.1)
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float JumpBufferTime = 0.1f; // no coyote time

	// ---- Input state ----
	uint8 bSprintKeyHeld : 1;
	uint8 bADSHeld : 1;
	uint8 bFireHeld : 1;
	uint8 bJumpKeyHeld : 1;

	// ---- FOV arbiter state ----
	float ADSAlpha = 0.f;
	float SprintKickAlpha = 0.f;
	float SlideKickAlpha = 0.f;

	// ---- Jump buffer / landing dip ----
	float LastJumpPressedTime = -1000.f;
	float FallStartPeakZ = 0.f;

	FTimerHandle SprintOutTimerHandle;
	FTimerHandle BufferedJumpClearHandle;

	uint8 CachedTeamId = 0;
};
