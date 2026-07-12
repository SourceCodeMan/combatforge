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
class UStaticMesh;
class USkeletalMesh;
class USkeletalMeshComponent;
class UAnimInstance;
class UMaterialInterface;
class USceneComponent;
class UPointLightComponent;
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

	// ---- Weapon-fire cosmetics (pkg-weapons calls these per shot) ----
	void  OnFireCosmetic();        // owning client: viewmodel recoil kick + first-person muzzle flash + light
	void  OnRemoteFireCosmetic();  // remote viewers: third-person muzzle flash + light at the shooter's marker

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

	/**
	 * Advances ADSAlpha toward IsADS() every Tick on EVERY role — the server
	 * feeds GetADSAlpha() into the authoritative spread cone, so the alpha can
	 * never live behind an IsLocallyControlled() gate.
	 */
	void UpdateADSAlpha(float DeltaSeconds);

	/** Single compose point for every FOV effect (04 §1.3). Local camera only. */
	void UpdateTargetFOV(float DeltaSeconds);

	/**
	 * M1 art pass: if the optional art meshes below are assigned, swap the graybox
	 * cubes for a real skeletal body / FP arms / weapon. Every branch is a no-op when
	 * its property is unset, so an unconfigured character is byte-identical to the graybox.
	 */
	void ApplyArtLoadout();

	/** Mounts the per-team skeletal body (Manny=0 / Quinn=1) with feet alignment; gated by CachedBodyTeamId. */
	void ApplyTeamBody(uint8 Team);

	/** Timer callbacks: snap flash/light off first; smoke lingers a beat longer (airsoft juice). */
	void ClearMuzzleFlash();
	void ClearMuzzleSmoke();

	/** Builds the primitive marker viewmodel (receiver/guard/barrel/stock/mag/grip) under Parent. */
	void BuildMarker(USceneComponent* Parent, const FString& Prefix, UStaticMesh* Cube, UStaticMesh* Cylinder,
		TArray<TObjectPtr<UStaticMeshComponent>>& OutParts, FVector& OutMuzzleLocal);
	UStaticMeshComponent* MakeGunPart(USceneComponent* Parent, const FString& CompName, UStaticMesh* PartMesh,
		UMaterialInterface* Mat, const FVector& RelLoc, const FVector& RelScale, const FRotator& RelRot);
	/** Runtime MIDs for the marker (dark gunmetal) + the flash blobs (emissive). Called from BeginPlay. */
	void SetupWeaponMaterials();

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

	// ---- Art loadout (M1 art pass; ALL optional — unset = the graybox cubes, unchanged) ----
	// Set these on a BP subclass, or via ctor FObjectFinder once the assets are imported.
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<USkeletalMeshComponent> FirstPersonArms;
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UStaticMeshComponent>  WeaponMeshComp;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<USkeletalMesh> ThirdPersonBodyMesh = nullptr;   // fallback TP body -> GetMesh()
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<USkeletalMesh> Team0BodyMesh = nullptr;         // team 0 body (Manny)
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<USkeletalMesh> Team1BodyMesh = nullptr;         // team 1 body (Quinn)
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TSubclassOf<UAnimInstance> ThirdPersonAnimClass;           // anim BP for the body
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<USkeletalMesh> FirstPersonArmsMesh = nullptr;   // FP arms -> FirstPersonArms
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UStaticMesh>   WeaponMesh = nullptr;            // rifle in hand (slice: static)
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FName WeaponAttachSocket = TEXT("hand_rSocket");           // socket on the TP body
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UMaterialInterface> TeamBodyMaterial = nullptr; // per-team body mat ("Color" param)
	bool bUsingArtBody = false;   // true once ThirdPersonBodyMesh mounted; gates the SetTeamColor/eliminate branches

	// ---- Weapon cosmetics: FP marker viewmodel + muzzle flash/smoke (engine primitives, no Niagara) ----
	UPROPERTY(VisibleAnywhere, Category="PF|Weapon") TObjectPtr<USceneComponent> ViewModelRoot;   // FP marker anchor (on camera)
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> MarkerPartsFP;                            // owner-only-see marker parts
	UPROPERTY(VisibleAnywhere, Category="PF|Weapon") TObjectPtr<UStaticMeshComponent> MuzzleFlashFP; // owner FP flash core
	UPROPERTY(VisibleAnywhere, Category="PF|Weapon") TObjectPtr<UStaticMeshComponent> MuzzleFlashTP; // viewers' flash core
	UPROPERTY(VisibleAnywhere, Category="PF|Weapon") TObjectPtr<UStaticMeshComponent> MuzzleSmokeFP; // owner FP smoke wisp
	UPROPERTY(VisibleAnywhere, Category="PF|Weapon") TObjectPtr<UStaticMeshComponent> MuzzleSmokeTP; // viewers' smoke wisp
	UPROPERTY(VisibleAnywhere, Category="PF|Weapon") TObjectPtr<UPointLightComponent> MuzzleLight;    // brief tight light pulse
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> MarkerMID;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> FlashMIDFP;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> FlashMIDTP;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> SmokeMIDFP;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> SmokeMIDTP;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> FlashMaterial;   // M_PF_Flash (or fallback)
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> SmokeMaterial;   // M_PF_MuzzleSmoke (or fallback)

	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float RecoilKickUU = 3.5f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float RecoilKickPitchDeg = 1.6f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float RecoilRecoverSpeed = 11.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float MuzzleFlashTime = 0.032f;   // snappy airsoft flash
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float MuzzleSmokeTime = 0.12f;    // short residual wisp
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float MuzzleLightIntensity = 9000.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float MuzzleLightRadius = 280.f;

	FVector ViewModelHomeLoc = FVector::ZeroVector;   // resting local location of ViewModelRoot
	FVector MuzzleLocalFP = FVector::ZeroVector;      // barrel tip in ViewModelRoot space
	FVector RecoilOffset = FVector::ZeroVector;       // decays to zero each tick (owner)
	float   RecoilPitch = 0.f;                        // deg, decays to zero
	FTimerHandle MuzzleFlashTimerHandle;
	FTimerHandle MuzzleSmokeTimerHandle;

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
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float JumpBufferTime = 0.1f; // no coyote time
	// Sprint-out delay (sprint -> first shot, 04 §1.1) is owned by UPFWeaponComponent::SprintOutTime (§3.4).

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
	uint8 CachedBodyTeamId = 255;   // gates the skeletal mesh swap so frequent SetTeamColor calls don't re-mount
	bool  bTeamAppearanceApplied = false;   // gates the (re)tint so frequent SetTeamColor calls don't rebuild MIDs
};
