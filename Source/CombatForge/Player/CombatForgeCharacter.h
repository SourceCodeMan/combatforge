// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Engine/TimerHandle.h"
#include "Player/PFCharacterCustomization.h"   // FPFCharacterConfig
#include "Combat/PFWeaponCatalog.h"            // FPFWeaponConfig
#include "Core/CombatForgeTypes.h"             // FPFKitRep
#include "CombatForgeCharacter.generated.h"

class UCameraComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UPFBuildComponent;
class UPFCharacterMovementComponent;
class UPFCombatAudio;
class UPFCombatVFX;
class UPFHealthComponent;
class UPFWeaponComponent;
class UNavigationInvokerComponent;
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
class COMBATFORGE_API ACombatForgeCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	// ObjectInitializer ctor is mandatory: swaps in UPFCharacterMovementComponent (§5.4).
	ACombatForgeCharacter(const FObjectInitializer& ObjectInitializer);

	// ---- Component accessors (cross-package; never null after construction) ----
	UPFCharacterMovementComponent* GetPFMovement() const;
	UPFWeaponComponent*  GetWeapon() const;
	UPFBuildComponent*   GetBuild()  const;
	UPFHealthComponent*  GetHealth() const;
	UPFCombatAudio*      GetCombatAudio() const;
	UPFCombatVFX*        GetCombatVFX() const;
	UCameraComponent*    GetFirstPersonCamera() const;

	// ---- ADS (owned here; CMC + weapon read through these) ----
	void  SetADS(bool bWantsADS);          // input entry; respects slide rule (queue during slide — 04 §1.2)
	bool  IsADS() const;                   // target state
	float GetADSAlpha() const;             // 0..1 transition alpha (0.18 in / 0.14 out) — weapon spread lerp input
	void  SetPreferredBaseFOV(float Fov);  // options menu hip FOV (80..110)

	// ---- Team + elimination cosmetics (pkg-weapons calls these) ----
	void  SetTeamColor(uint8 TeamId);      // MID tint on the graybox mesh
	void  SetEliminatedAppearance(bool bEliminated); // hide mesh; collision handled by health component
	FVector GetMuzzleLocation(bool bCosmetic) const; // FP viewmodel tip / TP rifle tip / eye-line fallback

	/** Eye world position (capsule top - eye offset) — used for aim-line raise, muzzle fallback, grenade spawn. */
	FVector GetEyeWorldLocation() const;

	// ---- Weapon-fire cosmetics (pkg-weapons calls these per shot) ----
	void  OnFireCosmetic(float RecoilScale = 1.f);   // owning client: viewmodel recoil kick (scaled by ADS + mag ramp)
	void  OnRemoteFireCosmetic();  // remote viewers: no flash (airsoft); reserved for future feel

	/** Hide FP viewmodel + TP rifle during BuildPhase (marker away while placing). */
	void UpdateBuildPhaseWeaponVisibility();

	// ---- AActor / ACharacter ----
	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
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
	void OnInteractPressed();      // F — nearest ammo barrel
	// Refill RPC lives on the PAWN (client-owned) not the barrel (GameMode-owned) so joined clients
	// route correctly; the barrel still does all authority validation in AuthorityInteract.
	UFUNCTION(Server, Reliable) void ServerRefillAtBarrel(class APFAmmoBarrel* Barrel);
	void OnFireSelectPressed();    // V — cycle fire mode
	void OnThrowFragPressed();     // E — throw frag
	void OnThrowSmokePressed();    // Q — throw smoke
	// Dev pose-tuning drag (gated by pf.WeaponDrag): hold MIDDLE MOUSE and move to slide the FP weapon in 3D.
	// Not ADS = edits the held pose (FPLoc); ADS = edits the ADS pose. Shift = depth, Ctrl = rotate. On
	// release, prints the pf.WeaponFP / pf.WeaponADS line to paste into PFWeaponCatalog.cpp.
	void OnWeaponDragPressed();
	void OnWeaponDragReleased();
	void TickWeaponDrag();          // applies the mouse delta while dragging (called from the local Tick block)
	void PrintWeaponPoseLine() const;

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
	 * TP rifle: hand-carry at rest; raise to eye/aim line while ADS or firing so the
	 * muzzle (and ball spawn) isn't at hip height. Always returns to the hand when idle.
	 */
	void UpdateWeaponHoldPose();

	/** True while ADS / fire held / brief post-shot window. */
	bool ShouldRaiseWeapon() const;

	/** Snap to resolved hand bone with grip offsets (idle carry). */
	void ApplyHandWeaponPose();

	/** Capsule/eye aim-line pose (shooting) — re-parented off the hand for this window only. */
	void ApplyRaisedWeaponPose();

	/**
	 * Quantum (no matching AnimBP): drive idle/walk/run via AnimSingleNodeInstance.
	 * Survival keeps ABP_Manny and skips this path.
	 */
	void UpdateSequenceLocomotion();

	/** Find a usable weapon attach bone/socket name on the live body. */
	FName ResolveWeaponAttachBone(const USkeletalMeshComponent* Body) const;

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
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UPFCombatVFX> CombatVFXComponent;
	UPROPERTY(Transient) TObjectPtr<UPFCharacterMovementComponent> PFMovement;
	// Generates the runtime navmesh in a radius around this pawn (host players + server bots both carry one),
	// so the dynamic mesh over the player-built arena exists wherever combatants actually are. See DefaultEngine.ini
	// [/Script/NavigationSystem.NavigationSystemV1] bGenerateNavigationOnlyAroundNavigationInvokers=True.
	UPROPERTY(VisibleAnywhere, Category="PF|AI") TObjectPtr<UNavigationInvokerComponent> NavInvoker;

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
	/** Locomotion sequences when no matching AnimBP (skeleton-gated PlayAnimation). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UAnimSequence> Team0IdleAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UAnimSequence> Team0WalkAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UAnimSequence> Team0RunAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UAnimSequence> Team1IdleAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UAnimSequence> Team1WalkAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UAnimSequence> Team1RunAnim = nullptr;

	// ---- Modular Bandit character spike (Phase 1; toggle with `pf.BanditChar 1`) ----
	// Base body carries SKM_Bandit_Skeleton + sequence locomotion; parts follow via Leader Pose.
	UPROPERTY(EditDefaultsOnly, Category="PF|Bandit") TObjectPtr<USkeletalMesh> BanditBodyMesh = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Bandit") TObjectPtr<UAnimSequence> BanditIdleAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Bandit") TObjectPtr<UAnimSequence> BanditWalkAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Bandit") TObjectPtr<UAnimSequence> BanditRunAnim = nullptr;
	// Rifle-hold locomotion borrowed from AnimStarterPack (the Bandit pack ships no armed anims). Plays on the
	// Bandit skeleton via a compatible-skeletons entry (Scripts/add_compatible_skeleton.py); pf.ArmedAnims 0
	// reverts to the unarmed A_MM_* set if the cross-skeleton playback looks wrong.
	UPROPERTY(EditDefaultsOnly, Category="PF|Bandit") TObjectPtr<UAnimSequence> ArmedIdleAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Bandit") TObjectPtr<UAnimSequence> ArmedWalkAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Bandit") TObjectPtr<UAnimSequence> ArmedRunAnim = nullptr;
	// 8-direction rifle sets (0=Fwd, clockwise 45° steps) — strafing plays real sidestep anims.
	UPROPERTY() TArray<TObjectPtr<UAnimSequence>> ArmedWalkDir;
	UPROPERTY() TArray<TObjectPtr<UAnimSequence>> ArmedJogDir;
	// Directional elimination reactions (0=front 1=right 2=back 3=left, relative to the shot).
	UPROPERTY() TArray<TObjectPtr<UAnimSequence>> DeathDirAnims;
	FTimerHandle DeathHideTimer;   // plays the fall, then hides the body
	bool bEliminatedAppearanceActive = false;   // gates UpdateSequenceLocomotion off the corpse
	UPROPERTY() TArray<TObjectPtr<USkeletalMeshComponent>> CharBaseComps;   // fixed base skin parts (head/legs)
	UPROPERTY() TArray<TObjectPtr<USkeletalMeshComponent>> CharSlotComps;   // one per PFChar customization slot
	UPROPERTY() TObjectPtr<UStaticMeshComponent> ArmbandMesh;              // team-colored band, left arm (team distinction)
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> ArmbandMID;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> ArmbandMeshR;            // right arm
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> ArmbandMIDR;
	FPFCharacterConfig ActiveCharConfig;                                   // current per-slot selection
	bool bBanditAssembled = false;
	FPFWeaponConfig ActiveWeaponConfig;                                    // current weapon selection

	// ---- Replicated kit (clothing + weapon). The class configs live in each player's LOCAL GameUserSettings,
	//      so the server can't read them — the owning client pushes its active class up (ServerSetKit) and the
	//      server replicates it to everyone. Fixes LAN: clients used to spawn with the HOST's weapon stats and
	//      default outfits on remote screens. Also powers the death-screen class switch.
	UPROPERTY(ReplicatedUsing=OnRep_Kit) FPFKitRep KitRep;
	UFUNCTION(Server, Reliable) void ServerSetKit(const FPFKitRep& NewKit);
	UFUNCTION() void OnRep_Kit();
	void ApplyKit();          // apply KitRep → ActiveCharConfig/ActiveWeaponConfig → visuals + weapon stats
	bool HasValidKit() const { return KitRep.CharParts.Num() > 0; }

	/** Phase-1 spike: mount the modular Bandit body + sequence-loco anims on GetMesh(). */
	void AssembleBanditCharacter();
	/** Phase-2: mount base skin + each config-selected overlay part via Leader Pose. */
	void ApplyCharacterConfig();

public:
	/** Set one slot's part index and re-apply (console/UI driven). */
	void SetCharSlot(int32 Slot, int32 Index);
	const FPFCharacterConfig& GetCharConfig() const { return ActiveCharConfig; }
	/** Reload the saved config from prefs and re-apply the overlay parts (menu edits an already-spawned pawn). */
	void ReapplyCharacterConfig();
	/** Owning client: build the kit from the ACTIVE class slot's saved prefs and push it to the server. */
	void PushLocalKit();
	/** Re-pick armed vs unarmed locomotion set from pf.ArmedAnims and re-arm a live pawn. Returns the idle. */
	UAnimSequence* RefreshBanditAnimSet();

	/** Apply the saved weapon selection: swap the FP viewmodel + TP weapon mesh/material/pose. */
	void ApplyWeaponLoadout();
	/** Reload the saved weapon config and re-apply (menu edits an already-spawned pawn). */
	void ReapplyWeaponLoadout();
	/** Live-tune the FP weapon pose (console: pf.WeaponFP) — the per-weapon poses need dialing in-editor. */
	void TuneWeaponFP(const FVector& Loc, const FRotator& Rot, float Scale, const FVector& Muzzle);
	/** Live-tune the per-weapon aim-down-sight pose (console: pf.WeaponADS). Hold right-click to preview. */
	void TuneWeaponADS(const FVector& Loc, const FRotator& Rot);

private:
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<USkeletalMesh> FirstPersonArmsMesh = nullptr;   // FP arms -> FirstPersonArms
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UStaticMesh>   WeaponMesh = nullptr;            // rifle in hand (slice: static)
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FName WeaponAttachSocket = TEXT("hand_r");                 // preferred hand bone
	// Grip in hand bone space (SM_Rifle / olive: local +Y barrel-forward). Tuned for hand_r.
	// Keep modest so Quantum/Survival hands don't shove the mesh into the skull.
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  WeaponRelativeLocation = FVector(-2.f, 8.f, -2.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FRotator WeaponRelativeRotation = FRotator(0.f, 90.f, 0.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  WeaponRelativeScale = FVector(0.85f);
	// Fallback when the mesh has no hand bone: hip-carry in mesh space (low — not chest/head).
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  WeaponMeshFallbackLocation = FVector(18.f, 22.f, 28.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FRotator WeaponMeshFallbackRotation = FRotator(5.f, 90.f, -10.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UMaterialInterface> TeamBodyMaterial = nullptr; // soft team tint fallback ("Color" param)
	// Optional single-slot overrides (mannequin only). Human models keep authored multi-slot mats.
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UMaterialInterface> Team0BodyMaterial = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UMaterialInterface> Team1BodyMaterial = nullptr;
	bool bUsingArtBody = false;   // true once ThirdPersonBodyMesh mounted; gates the SetTeamColor/eliminate branches
	/** True when team meshes are multi-material human packs — never wash with a single team MI. */
	bool bPreserveAuthoredMaterials = false;

	// ---- Weapon cosmetics: FP viewmodel + TP hand/raise (+ CO₂ muzzle VFX via UPFCombatVFX) ----
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
	/** How long the TP gun stays raised after a shot (covers auto-fire gaps + remotes). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float WeaponRaiseHoldOnShot = 0.45f;
	// Raised pose: mesh origin relative to eye (forward / right / down along aim basis).
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector WeaponRaisedFromEye = FVector(28.f, 14.f, -8.f);
	// FP viewmodel: hip-ish rest vs ADS. AdsLoc puts the TOP-SIGHT line on the camera axis: Y=-5.5 cancels the
	// rifle mesh's built-in +5.5 Y; Z lowered to -1.5 so the camera looks down the TOP sight, not the bore (the
	// iron sight sits above the barrel, so the whole gun drops that much). X~15 keeps the aperture in focus.
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") FVector ViewModelAdsLoc = FVector(15.f, -5.5f, -1.5f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") FRotator ViewModelAdsRot = FRotator(1.5f, 0.f, -1.5f);
	// Footstep stride (uu of ground travel between steps).
	UPROPERTY(EditDefaultsOnly, Category="PF|Audio") float FootstepStrideWalkUU = 165.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Audio") float FootstepStrideSprintUU = 130.f;

	// AI footstep noise: only RUNNING (sprinting) is loud enough for bots to hear 360° (walk/crouch = silent =
	// sight-only). Server emits a hearing event every FootstepNoiseInterval while sprinting, audible within
	// FootstepHearRangeUU (shorter than gunfire — footsteps don't carry as far).
	UPROPERTY(EditDefaultsOnly, Category="PF|AI") float FootstepNoiseInterval = 0.3f;
	UPROPERTY(EditDefaultsOnly, Category="PF|AI") float FootstepHearRangeUU = 2200.f;
	float FootstepNoiseTimer = 0.f;   // transient throttle

	FVector ViewModelHomeLoc = FVector::ZeroVector;   // resting local location of ViewModelRoot
	FVector MuzzleLocalFP = FVector::ZeroVector;      // barrel tip in ViewModelRoot space
	bool bWeaponDragging = false;                     // middle-mouse pose drag active (pf.WeaponDrag)

	// Procedural reload dip — FP viewmodel lowers + tilts while reloading (no skeletal reload anim).
	bool  bReloadDipActive = false;
	bool  bWasReloading = false;
	float ReloadDipElapsed = 0.f;
	float ReloadDipDuration = 1.f;
	FVector RecoilOffset = FVector::ZeroVector;       // decays to zero each tick (owner)
	float   RecoilPitch = 0.f;                        // deg, decays to zero

	// ---- Procedural viewmodel feel (owner only): sway / bob / breath / fire-kick / landing dip ----
	// Damped spring per channel (semi-implicit Euler). Slight underdamping (zeta < 1) gives the one-bounce
	// settle that reads as weapon WEIGHT; velocity impulses (not position sets) give fire kicks their snap.
	struct FPFSpring
	{
		float Pos = 0.f;
		float Vel = 0.f;
		void Update(float Target, float Dt, float Stiffness, float Zeta)
		{
			// Substepped (~120 Hz): a single Euler step at k=320 goes unstable/inverts below ~26 fps —
			// exactly the budget GPUs the frame cap work targets. Substepping keeps the feel identical
			// at 30 fps and 144 fps.
			const float Omega = FMath::Sqrt(FMath::Max(Stiffness, 1.f));
			const int32 N = FMath::Clamp(FMath::CeilToInt(Dt * 120.f), 1, 8);
			const float h = Dt / static_cast<float>(N);
			for (int32 i = 0; i < N; ++i)
			{
				const float Accel = -Stiffness * (Pos - Target) - 2.f * Zeta * Omega * Vel;
				Vel += Accel * h;
				Pos += Vel * h;
			}
		}
	};
	FPFSpring SwayYaw, SwayPitch, SwayX, SwayZ;             // look-lag rotation + translation
	FPFSpring KickBack, KickUp, KickPitch, KickYaw, KickRoll; // fire impulses
	FPFSpring LandDipSpring;                                  // landing dip (viewmodel; camera shake is separate)
	float BobPhase = 0.f;
	float BobAlpha = 0.f;      // smoothed 0..1 "moving & grounded" gate
	float BreathPhase = 0.f;
	float PrevCamYaw = 0.f;
	float PrevCamPitch = 0.f;
	bool  bCamRotInit = false;
	void UpdateViewmodelFeel(float DeltaSeconds, FVector& OutLoc, FRotator& OutRot);   // adds sway/bob/breath/kick/land terms
	float   WeaponRaiseHoldSec = 0.f;                 // countdown while briefly raised after shot
	float   FootstepDistanceAccum = 0.f;              // ground travel since last step (local)
	/** Sequence-driven locomotion (0=none, 1=idle, 2=walk, 3=run). */
	uint8   SeqLocoState = 0;
	bool    bSequenceLocoActive = false;              // Quantum single-node path
	FName   CachedWeaponAttachBone = NAME_None;       // resolved once per body

	// ---- Config (04 §1) — BaseFOV also set from options (PFUserPrefs) ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float BaseFOV = 105.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float ADSFOV = 58.f;   // tighter magnified sight picture
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float ADSInTime = 0.14f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float ADSOutTime = 0.14f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float SprintFOVKick = 6.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float SprintKickTime = 0.15f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float SlideFOVKick = 9.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float CameraEyeOffsetFromCapsuleTop = 10.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float CrouchCameraInterpSpeed = 150.f; // 30 uu over 0.2 s
	UPROPERTY(EditDefaultsOnly, Category="PF|Camera") float LandingDipMinFallUU = 300.f;
	/** Fall distance (uu) that is lethal → instant respawn. 3 build levels = 3 × 300. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Movement") float FallLethalHeightUU = 900.f;
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
