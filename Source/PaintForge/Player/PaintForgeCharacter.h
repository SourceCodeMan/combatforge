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
class UAnimSequence;
class UMaterialInterface;
class USceneComponent;
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
	FVector GetMuzzleLocation(bool bCosmetic) const; // FP viewmodel tip / TP rifle tip / eye-line fallback

	// ---- Weapon-fire cosmetics (pkg-weapons calls these per shot) ----
	void  OnFireCosmetic();        // owning client: viewmodel recoil kick only (airsoft — no flash)
	void  OnRemoteFireCosmetic();  // remote viewers: no flash (airsoft); reserved for future feel

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

	/**
	 * Sets TP rifle mesh + materials. Safe no-op if missing. Re-run after team body swaps.
	 */
	void AttachWeaponToHand();

	/**
	 * TP rifle: resting = snap to hand_r (in hands); raised = aim line while ADS / firing.
	 * Unarmed ABP has no rifle grip pose — hand attach is the idle stand-in until a rifle ABP lands.
	 */
	void UpdateWeaponHoldPose();

	/** True when the TP marker should leave the hand and raise to the aim line. */
	bool ShouldRaiseWeapon() const;

	/** Snap to hand_r with resting grip offsets (idle / not shooting). */
	void ApplyHandWeaponPose();

	/** World aim-line hold at the shoulder (only while ShouldRaiseWeapon). */
	void ApplyRaisedWeaponPose();

	/** Builds the primitive marker viewmodel (receiver/guard/barrel/stock/mag/grip) under Parent. */
	void BuildMarker(USceneComponent* Parent, const FString& Prefix, UStaticMesh* Cube, UStaticMesh* Cylinder,
		TArray<TObjectPtr<UStaticMeshComponent>>& OutParts, FVector& OutMuzzleLocal);
	UStaticMeshComponent* MakeGunPart(USceneComponent* Parent, const FString& CompName, UStaticMesh* PartMesh,
		UMaterialInterface* Mat, const FVector& RelLoc, const FVector& RelScale, const FRotator& RelRot);
	/** Runtime MIDs for the marker (dark gunmetal). Called from BeginPlay. */
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
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<USkeletalMesh> Team0BodyMesh = nullptr;         // team 0 = Quantum operator
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<USkeletalMesh> Team1BodyMesh = nullptr;         // team 1 = Survival character
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TSubclassOf<UAnimInstance> ThirdPersonAnimClass;           // mannequin ABP fallback only
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TSubclassOf<UAnimInstance> Team0AnimClass;                 // optional per-team AnimBP
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TSubclassOf<UAnimInstance> Team1AnimClass;
	/** Looping idle when no matching AnimBP (skeleton-gated PlayAnimation). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UAnimSequence> Team0IdleAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UAnimSequence> Team1IdleAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<USkeletalMesh> FirstPersonArmsMesh = nullptr;   // FP arms -> FirstPersonArms
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UStaticMesh>   WeaponMesh = nullptr;            // rifle in hand (slice: static)
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FName WeaponAttachSocket = TEXT("hand_r");                 // hand bone on the TP body
	// Resting grip in hand_r bone space (SM_Rifle: local +Y is barrel-forward).
	// Tuned so the stock sits in the palm — not through the torso / out the back.
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  WeaponRelativeLocation = FVector(3.f, -1.5f, 1.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FRotator WeaponRelativeRotation = FRotator(-5.f, 95.f, 8.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  WeaponRelativeScale = FVector(0.9f);
	// Raised aim-line offsets from capsule center (only while firing / ADS).
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector WeaponRaisedForward = FVector(28.f, 16.f, 48.f); // along aim X/Y + world Z
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UMaterialInterface> TeamBodyMaterial = nullptr; // soft team tint fallback ("Color" param)
	// Optional single-slot overrides (mannequin only). Human models keep authored multi-slot mats.
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UMaterialInterface> Team0BodyMaterial = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UMaterialInterface> Team1BodyMaterial = nullptr;
	bool bUsingArtBody = false;   // true once ThirdPersonBodyMesh mounted; gates the SetTeamColor/eliminate branches
	/** True when team meshes are multi-material human packs — never wash with a single team MI. */
	bool bPreserveAuthoredMaterials = false;

	// ---- Weapon cosmetics: FP viewmodel + TP hand/raise (airsoft — no muzzle flash) ----
	UPROPERTY(VisibleAnywhere, Category="PF|Weapon") TObjectPtr<USceneComponent> ViewModelRoot;   // FP marker anchor (on camera)
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> MarkerPartsFP;                            // owner-only-see marker parts
	UPROPERTY(VisibleAnywhere, Category="PF|Weapon") TObjectPtr<UStaticMeshComponent> RifleFPMesh; // real FP rifle (replaces the marker gun)
	UPROPERTY() TObjectPtr<UMaterialInterface> RifleMaterial;                                       // M_PF_Rifle (also overrides the TP weapon slot)
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> MarkerMID;

	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float RecoilKickUU = 3.5f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float RecoilKickPitchDeg = 1.6f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float RecoilRecoverSpeed = 11.f;
	// Local tip of SM_Rifle when barrel-forward is +Y (after TP world yaw -90).
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") FVector RifleMuzzleLocalTP = FVector(0.f, 58.f, 4.f);
	/** Brief raise after remote shot FX so viewers see the marker come up even without ADS rep. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float WeaponRaiseHoldOnShot = 0.35f;

	FVector ViewModelHomeLoc = FVector::ZeroVector;   // resting local location of ViewModelRoot
	FVector MuzzleLocalFP = FVector::ZeroVector;      // barrel tip in ViewModelRoot space
	FVector RecoilOffset = FVector::ZeroVector;       // decays to zero each tick (owner)
	float   RecoilPitch = 0.f;                        // deg, decays to zero
	float   WeaponRaiseHoldSec = 0.f;                 // countdown; keeps raised pose after a shot
	bool    bWeaponInRaisedPose = false;              // last applied pose (hand vs raised)

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
