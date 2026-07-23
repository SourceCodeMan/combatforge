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
	/** Reload begin/end pushes the current ADS intent into the movement stream (IsADS() self-suppresses
	 *  while WeaponComponent->bReloading). Called by UPFWeaponComponent so a mid-reload aim release is honored. */
	void  NotifyReloadStateChanged();
	/** Re-reads the hold-vs-toggle ADS pref (FPFUserPrefs::GetADSToggle). Called on possess and when the
	 *  options menu changes it live; clears any latched ADS on a mode change so you can't get stuck scoped. */
	void  RefreshADSToggleMode();
	/** Re-reads the hold-vs-toggle crouch pref (FPFUserPrefs::GetCrouchToggle). Called on possess and when
	 *  the options menu changes it live; clears latched crouch on a mode change so you can't get stuck low. */
	void  RefreshCrouchToggleMode();

	// ---- Team + elimination cosmetics (pkg-weapons calls these) ----
	void  SetTeamColor(uint8 TeamId);      // MID tint on the graybox mesh
	/** Hide/show the body for elimination; collision handled by health component. bPlayDeathAnim=false =
	 *  instant hide (late-join re-assert: the death is old news, don't replay the fall). */
	void  SetEliminatedAppearance(bool bEliminated, bool bPlayDeathAnim = true);
	/** BB/tracer origin. Prefers a Muzzle* socket on the gun mesh, else auto tip from mesh bounds
	 *  (longest horizontal axis) so every catalog weapon fires from its barrel without hand offsets. */
	FVector GetMuzzleLocation(bool bCosmetic) const;

	/** Eye world position (capsule top - eye offset) — used for aim-line raise, muzzle fallback, grenade spawn. */
	FVector GetEyeWorldLocation() const;

	// ---- Weapon-fire cosmetics (pkg-weapons calls these per shot) ----
	void  OnFireCosmetic(float RecoilScale = 1.f);   // owning client: viewmodel recoil kick (scaled by ADS + mag ramp)
	void  OnRemoteFireCosmetic();  // remote viewers: no flash (airsoft); reserved for future feel

	/** Hide FP viewmodel + TP rifle during BuildPhase (marker away while placing). */
	void UpdateBuildPhaseWeaponVisibility();

	// ---- Back-sling live tuning (pf.BackSling) ----
	FVector GetBackSlingOffset() const { return BackSlingOffset; }
	float   GetBackSlingTilt() const   { return BackSlingTiltDeg; }
	/** Apply new sling placement and re-attach immediately so the change is visible without a respawn. */
	void SetBackSling(const FVector& Offset, float TiltDeg);

	// ---- Demolition bomb (mid-field pickup only — not granted at spawn) ----
	bool IsCarryingBomb() const { return bCarryingBomb; }
	/** Authority: grant one plantable charge (from the mid-field floating pickup). */
	void GrantBombCharge();
	/** Authority: consume a carried charge (returns false if none). */
	bool ConsumeBombCharge();

	/**
	 * Local interact prompt for the combat HUD (e.g. "F to open" near a door).
	 * Empty when nothing usable is in range. Client display only.
	 */
	FString GetInteractPromptText() const;

	// ---- AActor / ACharacter ----
	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** Bots roll their random weapon here: BeginPlay runs pre-possession, so IsBotControlled() was false then. */
	virtual void PossessedBy(AController* NewController) override;
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
	UFUNCTION(Server, Reliable) void ServerToggleBuildDoor(class APFBuildPieceActor* Door);
	void OnFireSelectPressed();    // V — cycle fire mode
	void OnThrowFragPressed();     // E — throw frag
	void OnThrowSmokePressed();    // Q — throw smoke
	void OnMeleePressed();         // B / mouse-thumb — close-range melee tag ("you're out")
	// Server-authoritative melee: short forward trace from the camera; first live enemy pawn in range is
	// eliminated outright (the tag). Client requests; server re-validates the cooldown + range + team.
	UFUNCTION(Server, Reliable) void ServerMelee();
	/** Cosmetic punch: FP viewmodel jab + 3P raise. Multicast so remotes see the swing. */
	UFUNCTION(NetMulticast, Unreliable) void MulticastMeleeSwing();
	void PlayMeleeSwingLocal();

	// ---- Demolition bomb plant / defuse (charges from mid-field F pickup) ----
	void OnPlantPressed();         // G — plant a carried bomb on the aimed structural build piece (Combat only)
	void OnInteractReleased();     // F released — stop defusing (barrel refill is tap-only, unaffected)
	// Pawn-routed Server RPCs (the bomb + GameMode have no owning client connection — barrel pattern).
	UFUNCTION(Server, Reliable) void ServerPlantBomb(uint16 PieceId);
	UFUNCTION(Server, Reliable) void ServerBeginDefuse();
	UFUNCTION(Server, Reliable) void ServerEndDefuse();
	UFUNCTION(Server, Reliable) void ServerClaimBombPickup(class APFBombPickup* Pickup);
	// Dev pose-tuning drag (gated by pf.WeaponDrag): hold MIDDLE MOUSE.
	// Plain = translate, Shift = depth, Ctrl = pitch/yaw the muzzle (barrel aim), Alt = roll.
	// Hip vs ADS depends on aim state. Release logs paste-ready pf.WeaponFP / pf.WeaponADS lines.
	void OnWeaponDragPressed();
	void OnWeaponDragReleased();
	void TickWeaponDrag();          // applies the mouse delta while dragging (called from the local Tick block)

	/** Sprint-out raise timer elapsed — release the buffered fire (04 §1.1). */
	void OnSprintOutFinished();

	/** Deferred StopJumping after a buffered landing jump fired. */
	void ClearBufferedJump();

	/** Recomputes the CMC sprint/ADS intents from held keys + slide state. */
	void UpdateMovementIntents();

	/** Slide state delegate from the CMC (both sides; we only use it locally). */
	void HandleSlideStateChanged(bool bSliding);
	/** Mantle enter/exit — plays MM_WallJump (or Bandit jump fallback) for the climb. */
	void HandleMantleStateChanged(bool bMantling);

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

	// (ApplyRaisedWeaponPose was deleted 2026-07-18 — it was never called. UpdateWeaponHoldPose is the ONE
	//  live third-person weapon-pose path; edit that, not a helper that looks like it does the job.)

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
	/** Stowed / inactive gun on the back (primary when secondary drawn, or vice versa). */
	UPROPERTY(VisibleAnywhere, Category="PF|Components") TObjectPtr<UStaticMeshComponent>  BackWeaponMeshComp;
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
	// Unarmed punch clips for B-melee (MM_Attack_01..03). Empty = procedural-only fallback.
	UPROPERTY() TArray<TObjectPtr<UAnimSequence>> MeleeAnims;
	/** Ledge climb anim — mannequin MM_WallJump preferred; Bandit A_MM_Jump as skeleton-safe fallback. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Mantle") TObjectPtr<UAnimSequence> MantleAnim = nullptr;
	UPROPERTY(EditDefaultsOnly, Category="PF|Mantle") TObjectPtr<UAnimSequence> MantleAnimFallback = nullptr;
	bool bMantleAnimActive = false;
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
	/** Server RPC backing RequestResetToSpawn(): owning client → server teleport-to-spawn (heal + refill). */
	UFUNCTION(Server, Reliable) void ServerRequestResetToSpawn();
	/** Owning client → server: set this player's in-game name to their account display name (login profile).
	 *  Without it the PlayerState keeps an engine default and everyone shows the wrong / host name. */
	UFUNCTION(Server, Reliable) void ServerSetPlayerName(const FString& Name);
	void ApplyKit();          // apply KitRep → ActiveCharConfig/ActiveWeaponConfig → visuals + weapon stats
	bool HasValidKit() const { return KitRep.CharParts.Num() > 0; }

	// ---- Dual-weapon carry: primary + secondary (any catalog guns). Scroll swaps which is in hand;
	// the other rides the back sling. bSecondaryActive = secondary is the hand gun.
	UPROPERTY(ReplicatedUsing=OnRep_SecondaryActive) bool bSecondaryActive = false;
	UFUNCTION() void OnRep_SecondaryActive();
	UFUNCTION(Server, Reliable) void ServerSwapWeapon();
	uint8 StashHopper[2]  = { 0, 0 };     // saved mag per slot (0=primary, 1=secondary)
	int32 StashReserve[2] = { 0, 0 };     // saved reserve per slot
	bool  bStashValid[2]  = { false, false };
	double LastSwapTime = -100.0;
	static constexpr float SwapCooldown = 0.25f;   // debounce a multi-notch scroll into one swap
	FPFWeaponConfig PrimaryWeaponConfig;
	FPFWeaponConfig SecondaryWeaponConfig;
public:
	/** Owning client: scroll-wheel while alive → request a weapon swap (debounced). */
	void OnWeaponSwapInput();
	/** Server: force the primary weapon + full ammo (respawn). */
	void ResetToPrimaryWeapon();
	/** Options-menu "reset to spawn": owning client → server teleport-to-spawn (full heal + refill). No-op without a pawn. */
	void RequestResetToSpawn();

	/** Log paste-ready hip + ADS pose lines (includes WeaponId for catalog paste). Console: pf.Weapon dump. */
	void PrintWeaponPoseLine() const;
	/**
	 * Dev: equip a catalog gun in hand for pose tuning (pf.Weapon next/prev / cat idx / id).
	 * Turns on pf.WeaponDrag and off pf.WeaponAutoPose so middle-mouse drag edits catalog poses.
	 * Live drag edits are cached per WeaponId so cycling does not wipe them mid-session.
	 */
	void DevEquipCatalogWeapon(int32 Category, int32 Index);
	/** Step Dir through the flat catalog (all categories). Returns true if equipped. */
	bool DevCycleCatalogWeapon(int32 Dir);
private:
	/** Session-only pose overrides while tuning (key = WeaponId). Survives cycle; cleared on EndPlay. */
	struct FPFSessionWeaponPose
	{
		FVector  FPLoc = FVector::ZeroVector;
		FRotator FPRot = FRotator::ZeroRotator;
		float    FPScale = 1.f;
		FVector  MuzzleFP = FVector::ZeroVector;
		FVector  AdsLoc = FVector::ZeroVector;
		FRotator AdsRot = FRotator::ZeroRotator;
	};
	TMap<FName, FPFSessionWeaponPose> SessionWeaponPoses;
	void CacheCurrentWeaponPose();
	bool TryApplySessionWeaponPose(const FName& WeaponId, FVector& InOutFPLoc, FRotator& InOutFPRot,
		float& InOutFPScale, FVector& InOutMuzzle, FVector& InOutAdsLoc, FRotator& InOutAdsRot) const;

	/** Session-only THIRD-PERSON grip overrides (key = WeaponId). pf.WeaponTP records here so a live tune
	 *  survives respawn/class-cycle instead of silently reverting on the next ApplyWeaponLoadout. */
	struct FPFSessionWeaponTP
	{
		FVector  TPLoc = FVector::ZeroVector;
		FRotator TPRot = FRotator::ZeroRotator;
		float    TPScale = 0.85f;
	};
	TMap<FName, FPFSessionWeaponTP> SessionWeaponTPs;
	bool TryApplySessionWeaponTP(const FName& WeaponId, FVector& InOutLoc, FRotator& InOutRot,
		float& InOutScale) const;

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

	/** Apply kit weapons: active in hand + inactive on back sling. */
	void ApplyWeaponLoadout();
	/** Reload the saved weapon config and re-apply (menu edits an already-spawned pawn). */
	void ReapplyWeaponLoadout();
	/** Seat the stowed (non-active) gun on the back. */
	/** Exactly one owner-visible weapon in FP: drop a stale marker gun, and name anything unexpected. */
	void EnforceSingleFirstPersonWeapon();
	void AttachWeaponToBack(UStaticMesh* StowedMesh, UMaterialInterface* StowedMat);
	/** Live-tune the FP weapon pose (console: pf.WeaponFP) — the per-weapon poses need dialing in-editor. */
	void TuneWeaponFP(const FVector& Loc, const FRotator& Rot, float Scale, const FVector& Muzzle);
	/** Live-tune the per-weapon aim-down-sight pose (console: pf.WeaponADS). Hold right-click to preview. */
	void TuneWeaponADS(const FVector& Loc, const FRotator& Rot);
	/** Live-tune the THIRD-PERSON grip in hand_r (console: pf.WeaponTP) — what everyone else sees.
	 *  bRecordSession stores the values under this pawn's ACTIVE WeaponId so they survive respawn (the
	 *  console cmd sets it only on the tune-target pawn — mirrors don't pollute other weapons' entries). */
	void TuneWeaponTP(const FVector& Loc, const FRotator& Rot, float Scale, bool bRecordSession = true);
	/** Recompute the per-weapon TP grip layers (catalog row → auto+anchor → session tune) and re-pose.
	 *  Cheap: no mesh reload, no ammo/stat side effects — safe for live cvar/anchor refreshes. */
	void RecomputeTPGrip();
	/** Re-apply the back-sling transform (authored base + per-mesh auto correction) to the already-mounted
	 *  stowed mesh. Split out so the pf.WeaponAutoTP live toggle refreshes the sling too, not just the hand. */
	void ApplyBackSlingPose();
	/** Solve the global auto-TP anchor so this pawn's CURRENT (hand-tuned) grip becomes what the auto pose
	 *  produces for its equipped weapon — then every untuned weapon inherits the same correction.
	 *  Console: pf.WeaponTPCalibrate. Returns false if the mesh/bounds can't be computed. */
	bool CalibrateAutoTPFromCurrent();
	/** Drop the cached TP attach bones so they re-resolve next tick (pf.ArmedAnims / pf.WeaponBoneAttach toggles). */
	void InvalidateWeaponAttachBone() { CachedWeaponAttachBone = NAME_None; CachedHandLBone = NAME_None; }

private:
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<USkeletalMesh> FirstPersonArmsMesh = nullptr;   // FP arms -> FirstPersonArms
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") TObjectPtr<UStaticMesh>   WeaponMesh = nullptr;            // rifle in hand (slice: static)
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FName WeaponAttachSocket = TEXT("hand_r");                 // preferred hand bone
	// Grip in hand_r bone space (SM_Rifle family: local +Y = barrel-forward).
	// Bandit/Mannequin hang pose: palm faces roughly inward; these keep the stock in the palm and the
	// barrel forward-down along the thigh — NOT up into the armpit/neck (the dual-gun "neck+armpit" bug).
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  WeaponRelativeLocation = FVector(-3.f, 4.f, 2.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FRotator WeaponRelativeRotation = FRotator(10.f, 0.f, 90.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  WeaponRelativeScale = FVector(0.85f);
	// Fallback when the mesh has no hand bone: low hip-carry in mesh space (never chest/head).
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  WeaponMeshFallbackLocation = FVector(12.f, 18.f, 10.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FRotator WeaponMeshFallbackRotation = FRotator(5.f, 90.f, -10.f);
	// Back-sling pose: THE live values, applied as a plain relative transform on the spine bone by
	// ApplyBackSlingPose (the derived/actor-space placements were removed — two computed versions were
	// wrong in front of Tom). These are SPINE-BONE-space numbers despite pf.BackSling's Back/Side/Up arg
	// names; tune with pf.BackSling and paste what it prints. The per-mesh pivot/axis correction in
	// ApplyBackSlingPose layers ON TOP of these so one tune holds across all stowed meshes.
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  BackWeaponRelativeLocation = FVector(-18.f, 6.f, -6.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FRotator BackWeaponRelativeRotation = FRotator(0.f, 0.f, 75.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  BackWeaponRelativeScale = FVector(0.80f);
	/** Barrel angle off vertical in the back plane: 0 = straight down, 90 = horizontal. pf.BackSling tunes it. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") float    BackSlingTiltDeg = 35.f;
	/**
	 * Sling placement in the pawn's ACTOR frame, relative to the spine bone:
	 *   X = distance BEHIND the character, Y = lateral (+right), Z = vertical (negative drops it down the back).
	 * Actor space on purpose — bone space put this in the crotch once and at the neck once, because a spine
	 * bone's axes are not the character's. Live-tune with `pf.BackSling X Y Z Tilt`, then paste the printed
	 * values here (same workflow as pf.WeaponFP).
	 */
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector  BackSlingOffset = FVector(16.f, 0.f, -14.f);
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FName BackWeaponAttachBone = NAME_None; // resolved at runtime
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
	// Fallback TP tip in gun-mesh local space when auto bounds fail (graybox / missing mesh).
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") FVector RifleMuzzleLocalTP = FVector(0.f, 58.f, 4.f);
	/** Resolve world muzzle from a gun mesh component (socket → bounds tip → optional local fallback). */
	static FVector ResolveGunMuzzleWorld(const UStaticMeshComponent* Gun, const FVector& LocalFallback);
	/** How long the TP gun stays raised after a shot (covers auto-fire gaps + remotes). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Weapon") float WeaponRaiseHoldOnShot = 0.45f;
	// Raised pose: mesh origin relative to eye (forward / right / down along aim basis).
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") FVector WeaponRaisedFromEye = FVector(28.f, 14.f, -8.f);
	// Per-weapon barrel-axis correction for the raised (fire/ADS) TP pose, cached from the equipped Def in
	// ApplyWeaponLoadout. Default = SM_Rifle (+Y barrel). Pistols override so they don't render upside-down.
	float CachedTPRaisedYaw  = -90.f;
	float CachedTPRaisedRoll = 0.f;
	/** Last weapon id ApplyWeaponLoadout configured the WeaponComponent for — a SAME-id kit re-push
	 *  preserves the hopper instead of refilling (the free-instant-reload exploit); NAME_None = first apply. */
	FName LastAppliedWeaponId;
	/** Per-weapon third-person grip, cached from the catalog row on equip (pf.WeaponTP tunes these). */
	FVector  CachedTPLoc = FVector(-3.f, 4.f, 2.f);
	FRotator CachedTPRot = FRotator(10.f, 0.f, 90.f);
	float    CachedTPScale = 0.85f;
	/** Equipped mesh's grip point + barrel axis in MESH space (RecomputeTPGrip harvest) — drives the
	 *  per-tick left-palm aim. Zero = unavailable, aim no-ops. */
	FVector  CachedGripLocalMesh = FVector::ZeroVector;
	FVector  CachedBarrelAxisLocal = FVector::ZeroVector;
	/** Lazily-resolved left-hand bone for the palm aim (cleared with the attach bone). */
	FName    CachedHandLBone;
	// Firing shoulder lift when the ARMED idle isn't driving the arms up (arms hang -> gun sat at the hip).
	// Kept SMALL so a raised gun stays near the hand (hip/low-ready), not teleported to armpit/neck.
	// The old 42uu adaptive lift is what put guns on necks while the secondary sat on the spine.
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") float WeaponShoulderDropFromEyeUU = 28.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Art") float WeaponMaxShoulderLiftUU     = 16.f;
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
	/** Use the authored MuzzleLocalFP instead of the mesh-bounds auto-tip. Set for guns whose bounds fool the
	 *  geometry heuristic (the minigun: a fat multi-barrel cluster whose auto-tip lands at a bounds corner
	 *  → tracer from the top-right of the screen). Lets pf.WeaponFP muzzle tuning actually take effect too. */
	bool bMuzzleFromAuthoredFP = false;
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
	uint8 bADSHeld : 1;          // player's standing ADS intent (hold = button down; toggle = latched)
	uint8 bFireHeld : 1;
	uint8 bSprintOutTapBuffered : 1;   // press+release inside the sprint-out window -> one shot when it ends
	uint8 bJumpKeyHeld : 1;
	uint8 bADSToggleMode : 1;    // cached FPFUserPrefs::GetADSToggle(): false = hold, true = toggle
	uint8 bCrouchToggleMode : 1; // cached FPFUserPrefs::GetCrouchToggle(): false = hold, true = toggle

	// Demolition bomb: the bomb this pawn is currently holding F on (server-side), + plant debounce.
	TWeakObjectPtr<class APFBombActor> DefusingBomb;
	double LastPlantTime = -100.0;
	/** One plantable charge from the mid-field pickup (not granted at spawn). Replicated for HUD. */
	UPROPERTY(Replicated) bool bCarryingBomb = false;

	// Melee tag: last swing time. CLIENT and SERVER track separately so a listen-host
	// (where OnMeleePressed and ServerMelee_Implementation share one object) can't self-block.
	double LastMeleeTimeClient = -100.0;
	double LastMeleeTimeServer = -100.0;
	float  MeleeSwingAnimRemain = 0.f;            // procedural FP punch dip remaining (seconds)
	static constexpr float MeleeCooldown = 0.7f;   // seconds between swings
	static constexpr float MeleeRange    = 250.f;  // reach from the camera (uu)
	static constexpr float MeleeRadius   = 48.f;   // sweep radius so a near-miss still tags
	static constexpr float MeleeSwingAnimSec = 0.28f;

	// ---- AFK idle-kick (server-authoritative; anti-farm) ----
	// Poll the pawn's location + view rotation on the server; a REMOTE player who moves neither for
	// AfkKickSeconds is returned to their main menu (so they can't hold a slot / accrue XP while idle).
	// Never applies to bots (AIController) or the listen-server host / standalone (local controller).
	void TickServerAfk(float DeltaSeconds);
	// Position + aim alone are NOT enough: a player holding a Hardpoint/Domination point stays crouched and
	// still, and single-shot recoil (~0.17 deg climb) is both under the 1.0 deg aim gate AND fully recovered
	// between two 1 Hz polls — so a top-scoring point holder was being kicked mid-match. Instead of hooking
	// every action RPC across four components, fold the server-authoritative counters those actions already
	// mutate into one fingerprint: any change means the player did something real this second.
	uint32 ComputeServerActivityFingerprint() const;
	float    ServerLastActiveTime = -1.f;   // world-seconds of last detected activity (-1 = needs reseed on (re)spawn)
	float    ServerAfkPollAccum   = 0.f;    // throttles the poll to ~1 Hz
	FVector  ServerAfkLastLoc     = FVector::ZeroVector;
	FRotator ServerAfkLastAim     = FRotator::ZeroRotator;
	uint32   ServerAfkLastFingerprint = 0;  // last polled gameplay-action fingerprint (reseeded with the baseline)
	static constexpr float AfkKickSeconds = 180.f;   // 3 full minutes of complete idle

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
