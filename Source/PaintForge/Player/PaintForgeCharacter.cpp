// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PaintForgeCharacter.h"

#include "HAL/IConsoleManager.h"   // pf.BanditChar spike toggle

#include "PaintForge.h"
#include "Core/PaintForgeTypes.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerController.h"
#include "Player/PFCharacterMovementComponent.h"
#include "Player/PFCameraShakes.h"
#include "Input/PFInputConfig.h"
#include "Combat/PFWeaponComponent.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFCombatAudio.h"
#include "Combat/PFAmmoBarrel.h"
#include "Building/PFBuildComponent.h"
#include "Core/PFUserPrefs.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "EnhancedInputComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "InputActionValue.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

APaintForgeCharacter::APaintForgeCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UPFCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;

	bReplicates = true;
	SetNetUpdateFrequency(60.f);      // T28
	SetMinNetUpdateFrequency(30.f);

	bUseControllerRotationYaw = true; // first-person: capsule yaw follows camera
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	PFMovement = Cast<UPFCharacterMovementComponent>(GetCharacterMovement());

	UCapsuleComponent* Capsule = GetCapsuleComponent();
	Capsule->SetCollisionResponseToChannel(PF_ECC_Paintball, ECR_Block);   // §4.6
	Capsule->SetCollisionResponseToChannel(PF_ECC_BuildTrace, ECR_Ignore);

	// ---- First-person camera: capsule-top - 10 uu, 0-length boom equivalent ----
	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(Capsule);
	FirstPersonCamera->SetRelativeLocation(
		FVector(0.f, 0.f, Capsule->GetUnscaledCapsuleHalfHeight() - CameraEyeOffsetFromCapsuleTop));
	FirstPersonCamera->bUsePawnControlRotation = true;
	FirstPersonCamera->SetFieldOfView(BaseFOV);

	// ---- Graybox body: scaled engine cubes, team MID, hidden to owner ----
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MatFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(Capsule);
	if (CubeFinder.Succeeded())
	{
		BodyMesh->SetStaticMesh(CubeFinder.Object);
	}
	if (MatFinder.Succeeded())
	{
		BodyMesh->SetMaterial(0, MatFinder.Object);
	}
	// Torso+legs slab: 55 x 55 x 130 uu, top just below the head cube.
	BodyMesh->SetRelativeLocation(FVector(0.f, 0.f, -18.f));
	BodyMesh->SetRelativeScale3D(FVector(0.55f, 0.55f, 1.3f));
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BodyMesh->SetOwnerNoSee(true);

	HeadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeadMesh"));
	HeadMesh->SetupAttachment(Capsule);
	if (CubeFinder.Succeeded())
	{
		HeadMesh->SetStaticMesh(CubeFinder.Object);
	}
	if (MatFinder.Succeeded())
	{
		HeadMesh->SetMaterial(0, MatFinder.Object);
	}
	// Mask cube: 35 x 35 x 30 uu, inside the T1 mask band (capsule-top - 35).
	HeadMesh->SetRelativeLocation(FVector(0.f, 0.f, 68.f));
	HeadMesh->SetRelativeScale3D(FVector(0.35f, 0.35f, 0.3f));
	HeadMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HeadMesh->SetOwnerNoSee(true);

	// ACharacter's skeletal mesh stays asset-less; hide it defensively.
	if (GetMesh() != nullptr)
	{
		GetMesh()->SetVisibility(false);
	}

	// ---- Art loadout components (M1): created empty; ApplyArtLoadout assigns meshes if set ----
	FirstPersonArms = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("FirstPersonArms"));
	FirstPersonArms->SetupAttachment(FirstPersonCamera);
	FirstPersonArms->SetOnlyOwnerSee(true);          // FP arms: only the owning client sees them
	FirstPersonArms->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FirstPersonArms->SetVisibility(false);

	WeaponMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WeaponMeshComp"));
	WeaponMeshComp->SetupAttachment(GetMesh());      // re-attached to the hand socket in ApplyArtLoadout
	WeaponMeshComp->SetOwnerNoSee(true);             // slice: weapon rides the TP body only
	WeaponMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WeaponMeshComp->SetVisibility(false);

	// ---- First-person marker viewmodel: a rifle silhouette from engine primitives, owner-only-see.
	// This is what the player stares at every second — the single biggest "it's an FPS" signal. A real
	// weapon mesh drops in later by re-pointing the parts (or via the WeaponMesh art-loadout seam).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> RifleMeshFinder(TEXT("/Game/Weapons/Rifle/Mesh/SM_Rifle.SM_Rifle"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RifleMatFinder(TEXT("/Game/Weapons/Rifle/M_PF_Rifle.M_PF_Rifle"));
	UStaticMesh* CubeMesh = CubeFinder.Succeeded() ? CubeFinder.Object : nullptr;
	UStaticMesh* CylMesh = CylFinder.Succeeded() ? CylFinder.Object.Get() : CubeMesh;
	UMaterialInterface* GunBaseMat = MatFinder.Succeeded() ? MatFinder.Object : nullptr;

	ViewModelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ViewModelRoot"));
	ViewModelRoot->SetupAttachment(FirstPersonCamera);
	ViewModelHomeLoc = FVector(26.f, 9.f, -12.f);   // forward-right-down of the eye, classic viewmodel pose
	ViewModelRoot->SetRelativeLocation(ViewModelHomeLoc);

	// First-person weapon: the real rifle if available (Lyra SM_Rifle + generated M_PF_Rifle, self-contained),
	// otherwise the primitive marker gun. We build ONE or the OTHER — building no primitive means the
	// eliminate/respawn visibility propagate (SetChildVisibility) can never re-show an old primitive gun.
	// Airsoft: no muzzle flash / smoke / gunfire light components.
	if (UStaticMesh* RifleMesh = RifleMeshFinder.Succeeded() ? RifleMeshFinder.Object.Get() : nullptr)
	{
		RifleMaterial = RifleMatFinder.Succeeded() ? RifleMatFinder.Object.Get() : nullptr;
		RifleFPMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("RifleFPMesh"));
		RifleFPMesh->SetupAttachment(ViewModelRoot);
		RifleFPMesh->SetStaticMesh(RifleMesh);
		// SM_Rifle may still soft-ref missing Lyra MI_Weapon_Rifle — force every slot to our material
		// so packaged clients don't chase a missing package mid-match (LoadErrors + crash risk).
		if (RifleMaterial != nullptr)
		{
			const int32 Mats = RifleFPMesh->GetNumMaterials();
			for (int32 i = 0; i < Mats; ++i)
			{
				RifleFPMesh->SetMaterial(i, RifleMaterial);
			}
		}
		RifleFPMesh->SetOnlyOwnerSee(true);
		RifleFPMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		RifleFPMesh->SetCastShadow(false);
		// Held-rifle pose in ViewModelRoot space (+X forward, +Y right, +Z up). Mesh +Y = barrel.
		// Slightly tucked for "in hands"; ADS pulls ViewModelRoot toward iron-sight center.
		RifleFPMesh->SetRelativeLocation(FVector(3.f, 5.5f, -3.5f));
		RifleFPMesh->SetRelativeRotation(FRotator(-1.5f, -90.f, 1.5f));
		RifleFPMesh->SetRelativeScale3D(FVector(0.48f));
		// Barrel tip in ViewModelRoot space — cosmetic balls spawn from here (not under the gun).
		MuzzleLocalFP = FVector(42.f, 3.5f, -3.5f);
		WeaponMesh = RifleMesh;                      // third-person seam (shoulder pose in UpdateWeaponHoldPose)
	}
	else
	{
		BuildMarker(ViewModelRoot, TEXT("VM_"), CubeMesh, CylMesh, MarkerPartsFP, MuzzleLocalFP);
		for (TObjectPtr<UStaticMeshComponent>& Part : MarkerPartsFP)
		{
			if (Part != nullptr)
			{
				if (GunBaseMat != nullptr) { Part->SetMaterial(0, GunBaseMat); }
				Part->SetOnlyOwnerSee(true);   // the marker is the owner's first-person viewmodel
			}
		}
	}

	// Team models: each team is ONE human mesh for readability.
	//   Team 0 (blue)  = Quantum operator
	//   Team 1 (orange)= Survival character
	// Mannequin pack remains the graybox-friendly fallback if a pack is missing.
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> QuantumBodyFinder(
		TEXT("/Game/QuantumCharacter/Mesh/SKM_QuantumCharacter.SKM_QuantumCharacter"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> SurvivalBodyFinder(
		TEXT("/Game/Survival_Character/Meshes/SK_Survival_Character.SK_Survival_Character"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MannequinBodyFinder(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> QuinnBodyFinder(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple.SKM_Quinn_Simple"));
	// Quantum pack sequences (SK_Military skeleton) — used for sequence locomotion (no Quantum AnimBP).
	static ConstructorHelpers::FObjectFinder<UAnimSequence> QuantumIdleFinder(
		TEXT("/Game/QuantumCharacter/Demo/Animations/A_MM_Idle.A_MM_Idle"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> QuantumWalkFinder(
		TEXT("/Game/QuantumCharacter/Demo/Animations/A_MM_Walk_Fwd.A_MM_Walk_Fwd"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> QuantumRunFinder(
		TEXT("/Game/QuantumCharacter/Demo/Animations/A_MM_Run_Fwd.A_MM_Run_Fwd"));
	static ConstructorHelpers::FClassFinder<UAnimInstance> MannequinAnimFinder(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"));
	// Survival shares the mannequin skeleton with ABP_Manny (playtest-verified walk/run).
	static ConstructorHelpers::FClassFinder<UAnimInstance> SurvivalAnimFinder(
		TEXT("/Game/Survival_Character/Demo/Characters/Mannequins/Animations/ABP_Manny"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> SurvivalIdleFinder(
		TEXT("/Game/Survival_Character/Demo/Characters/Mannequins/Animations/Manny/MM_Idle.MM_Idle"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> OliveRifleFinder(
		TEXT("/Game/QuantumCharacter/Mesh/Rifle/SM_Rifle_Olive.SM_Rifle_Olive"));

	if (QuantumBodyFinder.Succeeded())
	{
		Team0BodyMesh = QuantumBodyFinder.Object;
		ThirdPersonBodyMesh = QuantumBodyFinder.Object;
		bPreserveAuthoredMaterials = true;   // multi-slot military materials — do not wash
	}
	else if (MannequinBodyFinder.Succeeded())
	{
		Team0BodyMesh = MannequinBodyFinder.Object;
		ThirdPersonBodyMesh = MannequinBodyFinder.Object;
	}

	if (SurvivalBodyFinder.Succeeded())
	{
		Team1BodyMesh = SurvivalBodyFinder.Object;
		bPreserveAuthoredMaterials = true;
		if (ThirdPersonBodyMesh == nullptr)
		{
			ThirdPersonBodyMesh = SurvivalBodyFinder.Object;
		}
	}
	else if (QuinnBodyFinder.Succeeded())
	{
		Team1BodyMesh = QuinnBodyFinder.Object;
	}
	else
	{
		Team1BodyMesh = Team0BodyMesh;
	}

	// Quantum: NEVER force mannequin ABP — skeletons don't match (playtest: frozen/broken anims).
	// Sequence locomotion (idle/walk/run) is driven in UpdateSequenceLocomotion.
	if (QuantumIdleFinder.Succeeded()) { Team0IdleAnim = QuantumIdleFinder.Object; }
	if (QuantumWalkFinder.Succeeded()) { Team0WalkAnim = QuantumWalkFinder.Object; }
	if (QuantumRunFinder.Succeeded())  { Team0RunAnim  = QuantumRunFinder.Object; }
	Team0AnimClass = nullptr;

	// Mannequin ABP only for actual mannequin meshes (fallback path).
	if (MannequinAnimFinder.Succeeded())
	{
		ThirdPersonAnimClass = MannequinAnimFinder.Class;
	}
	// Survival (team 1): mannequin-compatible ABP_Manny (playtest-verified).
	if (SurvivalAnimFinder.Succeeded()) { Team1AnimClass = SurvivalAnimFinder.Class; }
	if (SurvivalIdleFinder.Succeeded()) { Team1IdleAnim = SurvivalIdleFinder.Object; }

	// ---- Modular Bandit character spike (Phase 1; assembled only when `pf.BanditChar 1`) ----
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> BanditBodyFinder(
		TEXT("/Game/Bandits/Mesh/Body/SKM_Body.SKM_Body"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> BanditIdleFinder(
		TEXT("/Game/Bandits/Demo/Animations/A_MM_Idle.A_MM_Idle"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> BanditWalkFinder(
		TEXT("/Game/Bandits/Demo/Animations/A_MM_Walk_Fwd.A_MM_Walk_Fwd"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> BanditRunFinder(
		TEXT("/Game/Bandits/Demo/Animations/A_MM_Run_Fwd.A_MM_Run_Fwd"));
	if (BanditBodyFinder.Succeeded()) { BanditBodyMesh = BanditBodyFinder.Object; }
	if (BanditIdleFinder.Succeeded()) { BanditIdleAnim = BanditIdleFinder.Object; }
	if (BanditWalkFinder.Succeeded()) { BanditWalkAnim = BanditWalkFinder.Object; }
	if (BanditRunFinder.Succeeded())  { BanditRunAnim  = BanditRunFinder.Object; }

	// Config-driven modular slot components (base skin head/legs + one per PFChar customization slot). Part
	// meshes are assigned at assembly time from the active FPFCharacterConfig via the registry — no hardcoded
	// part paths, so all ~437 parts are reachable.
	auto MakePartComp = [this](const FString& CompName) -> USkeletalMeshComponent*
	{
		USkeletalMeshComponent* C = CreateDefaultSubobject<USkeletalMeshComponent>(*CompName);
		if (C != nullptr)
		{
			C->SetupAttachment(GetMesh());
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetVisibility(false);
			C->SetHiddenInGame(true);
		}
		return C;
	};
	for (int32 i = 0; i < 2; ++i)
	{
		if (USkeletalMeshComponent* C = MakePartComp(FString::Printf(TEXT("CharBase%d"), i)))
		{
			CharBaseComps.Add(C);
		}
	}
	for (int32 i = 0; i < PFChar::SlotCount(); ++i)
	{
		if (USkeletalMeshComponent* C = MakePartComp(FString::Printf(TEXT("CharSlot%d"), i)))
		{
			CharSlotComps.Add(C);
		}
	}

	// Team-colored armband — the team tell on the shared Bandit body. Engine cylinder wrapped thin around the
	// upper arm; re-parented to the arm bone, sized, and tinted at assembly time (see AssembleBanditCharacter).
	auto MakeArmband = [this, CylMesh](const TCHAR* Name) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* C = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		if (C != nullptr)
		{
			C->SetupAttachment(GetMesh());
			if (CylMesh != nullptr) { C->SetStaticMesh(CylMesh); }
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			C->SetVisibility(false);
			C->SetHiddenInGame(true);
			C->SetOwnerNoSee(true);   // hidden from the owning first-person view
		}
		return C;
	};
	ArmbandMesh  = MakeArmband(TEXT("Armband"));    // left arm
	ArmbandMeshR = MakeArmband(TEXT("ArmbandR"));   // right arm

	// Soft team-tint fallback (mannequin / graybox only).
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TeamBodyMatFinder(
		TEXT("/Game/Materials/M_PF_TeamBody.M_PF_TeamBody"));
	if (TeamBodyMatFinder.Succeeded())
	{
		TeamBodyMaterial = TeamBodyMatFinder.Object;
	}
	// Mannequin MIs only applied when NOT preserving authored human materials.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MannyMatFinder(
		TEXT("/Game/Characters/Mannequins/Materials/Manny/MI_Manny_01_New.MI_Manny_01_New"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> QuinnMatFinder(
		TEXT("/Game/Characters/Mannequins/Materials/Quinn/MI_Quinn_01.MI_Quinn_01"));
	if (!bPreserveAuthoredMaterials)
	{
		if (MannyMatFinder.Succeeded()) { Team0BodyMaterial = MannyMatFinder.Object; }
		if (QuinnMatFinder.Succeeded()) { Team1BodyMaterial = QuinnMatFinder.Object; }
	}

	// TP weapon: Quantum olive rifle if present, else Lyra SM_Rifle.
	if (OliveRifleFinder.Succeeded())
	{
		WeaponMesh = OliveRifleFinder.Object;
	}
	else if (RifleMeshFinder.Succeeded())
	{
		WeaponMesh = RifleMeshFinder.Object;
	}

	// Set the mesh's relative transform HERE (ctor), not just in BeginPlay: the Character Movement
	// Component captures this as its network-smoothing baseline for SIMULATED PROXIES. If it's only
	// set in BeginPlay, proxies smooth around a 0 baseline and the mannequin floats on other
	// players' screens. Mannequin origin is at its feet (measured minZ = 0), so offset = -halfHeight.
	if (ThirdPersonBodyMesh != nullptr && GetMesh() != nullptr)
	{
		GetMesh()->SetRelativeLocationAndRotation(
			FVector(0.f, 0.f, -Capsule->GetUnscaledCapsuleHalfHeight()),
			FRotator(0.f, -90.f, 0.f));
	}

	// ---- Cross-package components (own their replication flags in their ctors) ----
	WeaponComponent      = CreateDefaultSubobject<UPFWeaponComponent>(TEXT("WeaponComponent"));
	BuildComponent       = CreateDefaultSubobject<UPFBuildComponent>(TEXT("BuildComponent"));
	HealthComponent      = CreateDefaultSubobject<UPFHealthComponent>(TEXT("HealthComponent"));
	CombatAudioComponent = CreateDefaultSubobject<UPFCombatAudio>(TEXT("CombatAudioComponent"));

	bSprintKeyHeld = false;
	bADSHeld = false;
	bFireHeld = false;
	bJumpKeyHeld = false;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

UPFCharacterMovementComponent* APaintForgeCharacter::GetPFMovement() const { return PFMovement; }
UPFWeaponComponent*  APaintForgeCharacter::GetWeapon() const      { return WeaponComponent; }
UPFBuildComponent*   APaintForgeCharacter::GetBuild() const       { return BuildComponent; }
UPFHealthComponent*  APaintForgeCharacter::GetHealth() const      { return HealthComponent; }
UPFCombatAudio*      APaintForgeCharacter::GetCombatAudio() const { return CombatAudioComponent; }
UCameraComponent*    APaintForgeCharacter::GetFirstPersonCamera() const { return FirstPersonCamera; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void APaintForgeCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (PFMovement != nullptr)
	{
		PFMovement->OnSlideStateChanged.AddUObject(this, &APaintForgeCharacter::HandleSlideStateChanged);
	}

	FallStartPeakZ = GetActorLocation().Z;

	ApplyArtLoadout();       // swaps in real body/arms/weapon if assigned; no-op (graybox) otherwise
	SetupWeaponMaterials();  // dark gunmetal marker + emissive flash blobs
	ApplyWeaponLoadout();    // swap to the player's selected weapon (mesh/material/pose) — after the above

	// Soft wind/arena bed for the local player only (SFX volume scaled).
	if (IsLocallyControlled())
	{
		BaseFOV = FPFUserPrefs::GetFieldOfView();
		if (UPFCombatAudio* Audio = GetCombatAudio())
		{
			Audio->StartAmbientBed();
		}
	}
}

void APaintForgeCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PFMovement != nullptr)
	{
		PFMovement->OnSlideStateChanged.RemoveAll(this);
	}
	GetWorldTimerManager().ClearTimer(SprintOutTimerHandle);
	GetWorldTimerManager().ClearTimer(BufferedJumpClearHandle);
	if (UPFCombatAudio* Audio = GetCombatAudio())
	{
		Audio->StopAmbientBed();
	}

	Super::EndPlay(EndPlayReason);
}

void APaintForgeCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Track peak height while falling for the landing-dip threshold.
	if (PFMovement != nullptr && PFMovement->IsFalling())
	{
		FallStartPeakZ = FMath::Max(FallStartPeakZ, GetActorLocation().Z);
	}

	// ADS transition alpha advances on every role: the server reads
	// GetADSAlpha() for the authoritative spread cone (04 §2.3), so it must
	// track the move-stream ADS intent even for remotely controlled pawns.
	UpdateADSAlpha(DeltaSeconds);

	// Quantum (and any sequence-driven body): idle ↔ walk ↔ run without an AnimBP.
	UpdateSequenceLocomotion();

	// Build phase: no marker in hands (placement HUD has its own aim dot).
	UpdateBuildPhaseWeaponVisibility();

	// TP rifle: hand-carry, or eye-line raise while ADS / firing (then back to hand).
	if (WeaponRaiseHoldSec > 0.f)
	{
		WeaponRaiseHoldSec = FMath::Max(0.f, WeaponRaiseHoldSec - DeltaSeconds);
	}
	UpdateWeaponHoldPose();

	if (IsLocallyControlled())
	{
		// Slide/sprint state can change without new key events; keep the CMC
		// intents (and the queued-ADS rule) current every frame.
		UpdateMovementIntents();
		UpdateTargetFOV(DeltaSeconds);

		// Crouch camera smoothing: 88 -> 58 reads as 30 uu at 150 uu/s = 0.2 s.
		if (FirstPersonCamera != nullptr && GetCapsuleComponent() != nullptr)
		{
			const float TargetZ = GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - CameraEyeOffsetFromCapsuleTop;
			FVector Rel = FirstPersonCamera->GetRelativeLocation();
			Rel.Z = FMath::FInterpConstantTo(Rel.Z, TargetZ, DeltaSeconds, CrouchCameraInterpSpeed);
			FirstPersonCamera->SetRelativeLocation(Rel);
		}

		// Viewmodel: ADS pulls iron-sights toward center; recoil springs back each shot.
		if (ViewModelRoot != nullptr)
		{
			RecoilOffset = FMath::VInterpTo(RecoilOffset, FVector::ZeroVector, DeltaSeconds, RecoilRecoverSpeed);
			RecoilPitch = FMath::FInterpTo(RecoilPitch, 0.f, DeltaSeconds, RecoilRecoverSpeed);

			// Procedural reload animation: dip the marker down + tilt the muzzle for the reload's duration so a
			// reload reads visually (there is no skeletal reload anim on the FP mesh). Start on the reload edge.
			const bool bNowReloading = (WeaponComponent != nullptr) && WeaponComponent->bReloading;
			if (bNowReloading && !bWasReloading)
			{
				bReloadDipActive = true;
				ReloadDipElapsed = 0.f;
				ReloadDipDuration = (WeaponComponent != nullptr) ? FMath::Max(0.2f, WeaponComponent->ReloadTime) : 1.f;
			}
			bWasReloading = bNowReloading;

			FVector ReloadLoc = FVector::ZeroVector;
			float ReloadPitch = 0.f;
			if (bReloadDipActive)
			{
				ReloadDipElapsed += DeltaSeconds;
				const float A = FMath::Clamp(ReloadDipElapsed / FMath::Max(ReloadDipDuration, 0.01f), 0.f, 1.f);
				const float Dip = FMath::Sin(A * PI);   // 0 -> 1 -> 0 across the reload
				ReloadLoc = FVector(-4.f * Dip, 0.f, -10.f * Dip);   // pull in + down
				ReloadPitch = 20.f * Dip;                            // muzzle tilts down
				if (A >= 1.f)
				{
					bReloadDipActive = false;
				}
			}

			// Ease-out cubic (same curve as the FOV) so the gun snaps toward the sight and settles in lockstep
			// with the zoom instead of drifting linearly.
			const float RawAds = FMath::Clamp(ADSAlpha, 0.f, 1.f);
			const float Ads = 1.f - FMath::Cube(1.f - RawAds);
			const FVector Home = FMath::Lerp(ViewModelHomeLoc, ViewModelAdsLoc, Ads);
			const FRotator HomeRot = FMath::Lerp(FRotator::ZeroRotator, ViewModelAdsRot, Ads);
			ViewModelRoot->SetRelativeLocation(Home + RecoilOffset + ReloadLoc);
			ViewModelRoot->SetRelativeRotation(
				FRotator(RecoilPitch + HomeRot.Pitch + ReloadPitch, HomeRot.Yaw, HomeRot.Roll));
		}

		// Footsteps — local only, grounded movement.
		if (PFMovement != nullptr && !PFMovement->IsFalling() && !PFMovement->IsSliding())
		{
			const float Speed2D = GetVelocity().Size2D();
			if (Speed2D > 80.f)
			{
				const bool bSprint = PFMovement->IsSprintingEffective();
				const float Stride = bSprint ? FootstepStrideSprintUU : FootstepStrideWalkUU;
				FootstepDistanceAccum += Speed2D * DeltaSeconds;
				if (FootstepDistanceAccum >= Stride)
				{
					FootstepDistanceAccum = 0.f;
					if (UPFCombatAudio* Audio = GetCombatAudio())
					{
						Audio->PlayFootstep(bSprint);
					}
				}
			}
			else
			{
				FootstepDistanceAccum = 0.f;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Input setup
// ---------------------------------------------------------------------------

void APaintForgeCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(GetController());
	if (EIC == nullptr || PC == nullptr)
	{
		UE_LOG(PaintForgeLog, Warning,
			TEXT("APaintForgeCharacter::SetupPlayerInputComponent: missing EnhancedInputComponent or PaintForge PC"));
		return;
	}

	const UPFInputConfig* Cfg = PC->GetInputConfig();
	if (Cfg == nullptr)
	{
		UE_LOG(PaintForgeLog, Error, TEXT("SetupPlayerInputComponent: PC returned null UPFInputConfig"));
		return;
	}

	EIC->BindAction(Cfg->IA_Move, ETriggerEvent::Triggered, this, &APaintForgeCharacter::OnMoveInput);
	EIC->BindAction(Cfg->IA_Look, ETriggerEvent::Triggered, this, &APaintForgeCharacter::OnLookInput);
	EIC->BindAction(Cfg->IA_Jump, ETriggerEvent::Started, this, &APaintForgeCharacter::OnJumpPressed);
	EIC->BindAction(Cfg->IA_Jump, ETriggerEvent::Completed, this, &APaintForgeCharacter::OnJumpReleased);
	EIC->BindAction(Cfg->IA_Sprint, ETriggerEvent::Started, this, &APaintForgeCharacter::OnSprintPressed);
	EIC->BindAction(Cfg->IA_Sprint, ETriggerEvent::Completed, this, &APaintForgeCharacter::OnSprintReleased);
	EIC->BindAction(Cfg->IA_CrouchSlide, ETriggerEvent::Started, this, &APaintForgeCharacter::OnCrouchSlidePressed);
	EIC->BindAction(Cfg->IA_CrouchSlide, ETriggerEvent::Completed, this, &APaintForgeCharacter::OnCrouchSlideReleased);
	EIC->BindAction(Cfg->IA_Fire, ETriggerEvent::Started, this, &APaintForgeCharacter::OnFirePressed);
	EIC->BindAction(Cfg->IA_Fire, ETriggerEvent::Completed, this, &APaintForgeCharacter::OnFireReleased);
	EIC->BindAction(Cfg->IA_ADS, ETriggerEvent::Started, this, &APaintForgeCharacter::OnADSPressed);
	EIC->BindAction(Cfg->IA_ADS, ETriggerEvent::Completed, this, &APaintForgeCharacter::OnADSReleased);
	EIC->BindAction(Cfg->IA_Reload, ETriggerEvent::Started, this, &APaintForgeCharacter::OnReloadPressed);
	if (Cfg->IA_Interact)
	{
		EIC->BindAction(Cfg->IA_Interact, ETriggerEvent::Started, this, &APaintForgeCharacter::OnInteractPressed);
	}
	if (Cfg->IA_FireSelect)
	{
		EIC->BindAction(Cfg->IA_FireSelect, ETriggerEvent::Started, this, &APaintForgeCharacter::OnFireSelectPressed);
	}
	if (Cfg->IA_ThrowFrag)
	{
		EIC->BindAction(Cfg->IA_ThrowFrag, ETriggerEvent::Started, this, &APaintForgeCharacter::OnThrowFragPressed);
	}
	if (Cfg->IA_ThrowSmoke)
	{
		EIC->BindAction(Cfg->IA_ThrowSmoke, ETriggerEvent::Started, this, &APaintForgeCharacter::OnThrowSmokePressed);
	}

	// pkg-building owns every IMC_Build action (§3.3 / §3.5).
	if (BuildComponent != nullptr)
	{
		BuildComponent->BindInput(EIC, Cfg);
	}
}

// ---------------------------------------------------------------------------
// Movement input
// ---------------------------------------------------------------------------

void APaintForgeCharacter::OnMoveInput(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (Controller == nullptr)
	{
		return;
	}
	const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
	AddMovementInput(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y), Axis.X);
}

void APaintForgeCharacter::OnLookInput(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y); // Y already negated + scaled by the mapping modifiers
}

void APaintForgeCharacter::OnJumpPressed()
{
	bJumpKeyHeld = true;
	LastJumpPressedTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	Jump(); // no coyote time; pre-landing presses are re-armed in Landed()
}

void APaintForgeCharacter::OnJumpReleased()
{
	bJumpKeyHeld = false;
	StopJumping();
}

void APaintForgeCharacter::OnSprintPressed()
{
	bSprintKeyHeld = true;
	UpdateMovementIntents();
}

void APaintForgeCharacter::OnSprintReleased()
{
	bSprintKeyHeld = false;
	UpdateMovementIntents();
}

void APaintForgeCharacter::OnCrouchSlidePressed()
{
	if (PFMovement != nullptr)
	{
		PFMovement->OnCrouchSlidePressed();
	}
}

void APaintForgeCharacter::OnCrouchSlideReleased()
{
	if (PFMovement != nullptr)
	{
		PFMovement->OnCrouchSlideReleased();
	}
}

// ---------------------------------------------------------------------------
// Fire / ADS gating (04 §1.1: sprint blocks fire/ADS; sprint-out 0.18 s)
// ---------------------------------------------------------------------------

void APaintForgeCharacter::OnFirePressed()
{
	bFireHeld = true;

	// Capture sprint state BEFORE the intent update cancels it.
	const bool bNeedsRaise = (PFMovement != nullptr) && PFMovement->IsSprintingEffective();
	UpdateMovementIntents(); // fire held -> sprint intent drops immediately

	if (bNeedsRaise)
	{
		const float SprintOutDelay = (WeaponComponent != nullptr) ? WeaponComponent->SprintOutTime : 0.18f;
		GetWorldTimerManager().SetTimer(SprintOutTimerHandle, this,
			&APaintForgeCharacter::OnSprintOutFinished, SprintOutDelay, false);
	}
	else if (!GetWorldTimerManager().IsTimerActive(SprintOutTimerHandle))
	{
		if (WeaponComponent != nullptr)
		{
			WeaponComponent->StartFire();
		}
	}
}

void APaintForgeCharacter::OnSprintOutFinished()
{
	// Buffered fire releases when the raise timer ends (04 §1.1).
	if (bFireHeld && WeaponComponent != nullptr)
	{
		WeaponComponent->StartFire();
	}
}

void APaintForgeCharacter::OnFireReleased()
{
	bFireHeld = false;
	GetWorldTimerManager().ClearTimer(SprintOutTimerHandle);
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->StopFire();
	}
	UpdateMovementIntents(); // shift still held -> sprint resumes
}

void APaintForgeCharacter::OnADSPressed()
{
	SetADS(true);
}

void APaintForgeCharacter::OnADSReleased()
{
	SetADS(false);
}

void APaintForgeCharacter::OnReloadPressed()
{
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->StartReload();
	}
}

void APaintForgeCharacter::OnInteractPressed()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	// Nearest available ammo barrel in interact range.
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	APFAmmoBarrel* Best = nullptr;
	float BestDistSq = FMath::Square(220.f);
	const FVector Me = GetActorLocation();
	for (TActorIterator<APFAmmoBarrel> It(World); It; ++It)
	{
		APFAmmoBarrel* Barrel = *It;
		if (!Barrel || !Barrel->IsAvailable())
		{
			continue;
		}
		const float D = FVector::DistSquared(Me, Barrel->GetActorLocation());
		if (D <= BestDistSq)
		{
			BestDistSq = D;
			Best = Barrel;
		}
	}
	if (Best)
	{
		Best->LocalRequestInteract();
	}
}

void APaintForgeCharacter::OnFireSelectPressed()
{
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->CycleFireMode();
	}
}

void APaintForgeCharacter::OnThrowFragPressed()
{
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->StartThrow(EPFGrenadeType::Frag);
	}
}

void APaintForgeCharacter::OnThrowSmokePressed()
{
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->StartThrow(EPFGrenadeType::Smoke);
	}
}

void APaintForgeCharacter::SetADS(bool bWantsADS)
{
	// ADS input during a slide queues: bADSHeld stays true and IsADS() flips
	// on its own the moment the slide ends (04 §1.2).
	bADSHeld = bWantsADS;
	UpdateMovementIntents();
}

bool APaintForgeCharacter::IsADS() const
{
	if (PFMovement == nullptr)
	{
		return bADSHeld;
	}
	// Owning client: the raw held key drives the state (and feeds the
	// compressed flag via UpdateMovementIntents). Server / proxies: bADSHeld
	// never leaves the owning client, so read the intent back out of the move
	// stream (FLAG_Custom_1) — this keeps the server's spread cone identical
	// to the one the client predicted with.
	const bool bHeld = IsLocallyControlled() ? (bADSHeld != 0) : PFMovement->WantsToADS();
	return bHeld && !PFMovement->IsSliding();
}

float APaintForgeCharacter::GetADSAlpha() const
{
	return ADSAlpha;
}

void APaintForgeCharacter::SetPreferredBaseFOV(float Fov)
{
	BaseFOV = FMath::Clamp(Fov, 80.f, 110.f);
}

void APaintForgeCharacter::UpdateMovementIntents()
{
	if (PFMovement == nullptr)
	{
		return;
	}
	// Sprint blocks fire and ADS; fire/ADS input cancels sprint immediately and
	// sprint resumes when both are released while shift is still held (04 §1.1).
	// CONTRACT-GAP: 04 doesn't state what a sprint press does mid-burst; we let
	// the held fire/ADS win (sprint stays suppressed until they release).
	const bool bSprintIntent = bSprintKeyHeld && !bADSHeld && !bFireHeld;
	PFMovement->SetWantsToSprint(bSprintIntent);
	PFMovement->SetWantsToADS(IsADS());
}

void APaintForgeCharacter::HandleSlideStateChanged(bool /*bSliding*/)
{
	// Queued ADS applies on slide end; FOV kick state is polled in Tick.
	if (IsLocallyControlled())
	{
		UpdateMovementIntents();
	}
}

// ---------------------------------------------------------------------------
// FOV arbiter (04 §1.3) — the single compose point; effects never fight
// ---------------------------------------------------------------------------

void APaintForgeCharacter::UpdateADSAlpha(float DeltaSeconds)
{
	// Linear 0.18 s in / 0.14 s out; the ease-out cubic is applied at compose
	// time (FOV) and by the spread lerp consumer. Runs on every role — see Tick.
	const float Target = IsADS() ? 1.f : 0.f;
	const float Rate = (Target > ADSAlpha)
		? (ADSInTime > 0.f ? 1.f / ADSInTime : BIG_NUMBER)
		: (ADSOutTime > 0.f ? 1.f / ADSOutTime : BIG_NUMBER);
	ADSAlpha = FMath::FInterpConstantTo(ADSAlpha, Target, DeltaSeconds, Rate);
}

void APaintForgeCharacter::UpdateTargetFOV(float DeltaSeconds)
{
	if (FirstPersonCamera == nullptr)
	{
		return;
	}

	// Sprint kick: +6 over 0.15 s.
	{
		const float Target = (PFMovement != nullptr && PFMovement->IsSprintingEffective()) ? 1.f : 0.f;
		const float Rate = SprintKickTime > 0.f ? 1.f / SprintKickTime : BIG_NUMBER;
		SprintKickAlpha = FMath::FInterpConstantTo(SprintKickAlpha, Target, DeltaSeconds, Rate);
	}

	// Slide kick: +9 during the glide phase, lerped out with the friction ramp.
	{
		const float Target = (PFMovement != nullptr && PFMovement->IsSlideGlidePhase()) ? 1.f : 0.f;
		const float Rate = (Target > SlideKickAlpha)
			? 10.f                                                    // snap in with the boost
			: (PFMovement != nullptr && PFMovement->SlideFrictionRampTime > 0.f
				? 1.f / PFMovement->SlideFrictionRampTime : BIG_NUMBER); // out over the ramp (0.5 s)
		SlideKickAlpha = FMath::FInterpConstantTo(SlideKickAlpha, Target, DeltaSeconds, Rate);
	}

	// Ease-out cubic on the ADS transition (04 §1.4).
	const float Eased = 1.f - FMath::Cube(1.f - ADSAlpha);

	float FOV = FMath::Lerp(BaseFOV, ADSFOV, Eased);
	FOV += (1.f - Eased) * (SprintFOVKick * SprintKickAlpha + SlideFOVKick * SlideKickAlpha);

	FirstPersonCamera->SetFieldOfView(FOV);
}

// ---------------------------------------------------------------------------
// Landing: dip shake + jump buffer
// ---------------------------------------------------------------------------

void APaintForgeCharacter::OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PrevMovementMode, PreviousCustomMode);

	if (PFMovement != nullptr && PFMovement->IsFalling())
	{
		FallStartPeakZ = GetActorLocation().Z;
	}
}

void APaintForgeCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	// Landing dip on falls > 300 uu (04 §1.3), local camera only.
	const float FallDistance = FallStartPeakZ - GetActorLocation().Z;
	if (FallDistance > LandingDipMinFallUU && IsLocallyControlled())
	{
		if (APlayerController* PC = Cast<APlayerController>(GetController()))
		{
			if (PC->PlayerCameraManager != nullptr)
			{
				PC->PlayerCameraManager->StartCameraShake(UPFLandShake::StaticClass());
			}
		}
	}

	// Lethal fall from 3+ build levels (900 uu): authority only → elim + instant respawn path.
	if (HasAuthority() && FallDistance >= FallLethalHeightUU)
	{
		if (UPFHealthComponent* HPComp = GetHealth())
		{
			if (!HPComp->bEliminated && HPComp->HP > 0)
			{
				// Only during live combat so lobby/build falls are free.
				if (const UWorld* World = GetWorld())
				{
					if (const APaintForgeGameState* GS = World->GetGameState<APaintForgeGameState>())
					{
						if (GS->Phase == EPFMatchPhase::Combat && GS->RoundState == EPFRoundState::Live)
						{
							HPComp->ApplyFallDeath();
						}
					}
				}
			}
		}
	}

	FallStartPeakZ = GetActorLocation().Z;

	// 0.1 s pre-landing jump buffer, no coyote time (04 §1.1).
	if (GetWorld() != nullptr
		&& (GetWorld()->GetTimeSeconds() - LastJumpPressedTime) <= JumpBufferTime
		&& !bPressedJump)
	{
		Jump();
		if (!bJumpKeyHeld)
		{
			// Re-armed from a released tap: clear the pressed state right after
			// the jump consumes it so we don't hold a phantom jump.
			GetWorldTimerManager().SetTimer(BufferedJumpClearHandle, this,
				&APaintForgeCharacter::ClearBufferedJump, 0.15f, false);
		}
	}
}

void APaintForgeCharacter::ClearBufferedJump()
{
	if (!bJumpKeyHeld)
	{
		StopJumping();
	}
}

// ---------------------------------------------------------------------------
// Team + elimination cosmetics
// ---------------------------------------------------------------------------

void APaintForgeCharacter::AssembleBanditCharacter()
{
	USkeletalMeshComponent* Base = GetMesh();
	if (Base == nullptr || BanditBodyMesh == nullptr)
	{
		return;
	}

	// Route the Bandit sequences through the existing sequence-loco path (both teams — global test toggle).
	Team0IdleAnim = BanditIdleAnim; Team0WalkAnim = BanditWalkAnim; Team0RunAnim = BanditRunAnim;
	Team1IdleAnim = BanditIdleAnim; Team1WalkAnim = BanditWalkAnim; Team1RunAnim = BanditRunAnim;

	// Base body carries the skeleton + animation (single-node + UpdateSequenceLocomotion, like Quantum).
	Base->SetSkeletalMeshAsset(BanditBodyMesh);
	Base->Stop();
	Base->SetAnimInstanceClass(nullptr);
	Base->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	CachedWeaponAttachBone = NAME_None;
	SeqLocoState = 0;
	bSequenceLocoActive = true;
	bUsingArtBody = true;
	if (BanditIdleAnim != nullptr)
	{
		Base->PlayAnimation(BanditIdleAnim, /*bLooping=*/true);
		SeqLocoState = 1;
	}

	// Modular parts assembled from the player's saved character (SKM_Bandit_Skeleton -> Leader Pose).
	// Only the LOCAL human player wears the saved custom character; bots and other players get the standard
	// default look. (Per-player replicated configs are a later phase — the local config is per-machine for now.)
	if (ActiveCharConfig.Slots.Num() == 0)
	{
		const bool bLocalHuman = (GetController() != nullptr) && GetController()->IsLocalPlayerController();
		ActiveCharConfig = bLocalHuman ? PFChar::LoadConfig() : PFChar::DefaultConfig();
	}
	ApplyCharacterConfig();

	// Align: face +X, feet at capsule bottom (handles pelvis-origin packs).
	Base->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	if (const UCapsuleComponent* Cap = GetCapsuleComponent())
	{
		const float MeshMinZ = BanditBodyMesh->GetBounds().GetBox().Min.Z;
		const float FeetZ = -Cap->GetUnscaledCapsuleHalfHeight() - MeshMinZ;
		Base->SetRelativeLocation(FVector(0.f, 0.f, FeetZ));
	}
	Base->SetVisibility(true);
	Base->SetHiddenInGame(false);
	Base->SetOwnerNoSee(true);
	Base->SetCastShadow(true);
	if (BodyMesh != nullptr) { BodyMesh->SetVisibility(false); }
	if (HeadMesh != nullptr) { HeadMesh->SetVisibility(false); }

	// Re-seat the rifle on the new (Bandit) skeleton's hand bone — otherwise GetMuzzleLocation falls back to
	// the eye line and BBs spawn from the head. Bandit skeleton is Mannequin-compatible, so hand_r resolves.
	AttachWeaponToHand();

	// Team-colored armbands on BOTH upper arms. Bandit skeleton is Mannequin-compatible, so upperarm_l/_r resolve;
	// if a pack lacks a bone the band simply rides the mesh root (still visible, just not on the arm).
	auto MountArmband = [this, Base](UStaticMeshComponent* Band, TObjectPtr<UMaterialInstanceDynamic>& MID,
		const TCHAR* Bone, float BicepDir)
	{
		if (Band == nullptr)
		{
			return;
		}
		Band->AttachToComponent(Base, FAttachmentTransformRules::KeepRelativeTransform, Bone);
		// The right arm bone mirrors the left, so its +X runs toward the neck — flip the offset per side so the
		// band sits on the bicep on BOTH arms (left = +X down the arm, right = -X down the arm).
		Band->SetRelativeLocation(FVector(16.f * BicepDir, 0.f, 0.f));
		Band->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));     // cylinder axis -> along the arm bone
		Band->SetRelativeScale3D(FVector(0.16f, 0.16f, 0.045f)); // thin band, ~8 cm radius
		if (MID == nullptr && TeamBodyMaterial != nullptr)
		{
			MID = Band->CreateDynamicMaterialInstance(0, TeamBodyMaterial);
		}
		if (MID != nullptr)
		{
			MID->SetVectorParameterValue(TEXT("Color"), PFColors::ForTeam(CachedBodyTeamId));
		}
		Band->SetVisibility(true);
		Band->SetHiddenInGame(false);
		Band->SetOwnerNoSee(true);
	};
	MountArmband(ArmbandMesh, ArmbandMID, TEXT("upperarm_l"), 1.f);
	MountArmband(ArmbandMeshR, ArmbandMIDR, TEXT("upperarm_r"), -1.f);

	bBanditAssembled = true;
	UE_LOG(PaintForgeLog, Log, TEXT("AssembleBanditCharacter: mounted Bandit body + %d slot comps (%d parts in registry)."),
		CharSlotComps.Num(), PFChar::TotalPartCount());
}

void APaintForgeCharacter::ApplyCharacterConfig()
{
	USkeletalMeshComponent* Base = GetMesh();
	if (Base == nullptr)
	{
		return;
	}
	auto Mount = [Base](USkeletalMeshComponent* C, USkeletalMesh* M)
	{
		if (C == nullptr)
		{
			return;
		}
		if (M != nullptr)
		{
			C->SetSkeletalMeshAsset(M);
			C->SetLeaderPoseComponent(Base);
			C->SetVisibility(true);
			C->SetHiddenInGame(false);
			C->SetOwnerNoSee(true);
			C->SetCastShadow(true);
		}
		else
		{
			C->SetSkeletalMeshAsset(nullptr);
			C->SetVisibility(false);
			C->SetHiddenInGame(true);
		}
	};

	const TArray<FSoftObjectPath>& BaseP = PFChar::BaseParts();
	for (int32 i = 0; i < CharBaseComps.Num(); ++i)
	{
		USkeletalMesh* M = BaseP.IsValidIndex(i) ? Cast<USkeletalMesh>(BaseP[i].TryLoad()) : nullptr;
		Mount(CharBaseComps[i], M);
	}
	for (int32 s = 0; s < CharSlotComps.Num(); ++s)
	{
		const int32 Sel = ActiveCharConfig.Slots.IsValidIndex(s) ? ActiveCharConfig.Slots[s] : -1;
		USkeletalMesh* M = (Sel >= 0) ? PFChar::LoadPart(s, Sel) : nullptr;
		Mount(CharSlotComps[s], M);
	}
}

void APaintForgeCharacter::ReapplyCharacterConfig()
{
	if (!bBanditAssembled)
	{
		return;   // only meaningful once the modular character is mounted (pf.BanditChar)
	}
	ActiveCharConfig = PFChar::LoadConfig();
	ApplyCharacterConfig();
}

void APaintForgeCharacter::ApplyWeaponLoadout()
{
	ActiveWeaponConfig = PFWeapon::LoadConfig();
	const FPFWeaponDef& Def = PFWeapon::Weapon(ActiveWeaponConfig.Category, ActiveWeaponConfig.Index);
	UStaticMesh* WpnMesh = PFWeapon::LoadMesh(Def);
#if !UE_BUILD_SHIPPING
	// Confirm what actually equipped (category logic is correct; if a weapon LOOKS wrong it's the per-weapon
	// FP scale/pose, not the selection — tune with pf.WeaponFP).
	if (IsLocallyControlled() && GEngine != nullptr)
	{
		GEngine->AddOnScreenDebugMessage(-1, 4.f, FColor::Yellow, FString::Printf(TEXT("Weapon: %s / %s%s"),
			*PFWeapon::CategoryLabel(ActiveWeaponConfig.Category), Def.DisplayName,
			WpnMesh ? TEXT("") : TEXT("  [mesh missing]")));
	}
#endif
	if (WpnMesh == nullptr)
	{
		return;   // asset missing — keep the current weapon
	}
	UMaterialInterface* Mat = PFWeapon::LoadMaterial(Def);   // nullptr = preserve the mesh's authored materials
	RifleMaterial = Mat;   // AttachWeaponToHand + the FP block both key off this (null clears overrides)

	// First-person viewmodel: swap mesh, material (or revert to authored), and the per-weapon pose.
	if (RifleFPMesh != nullptr)
	{
		RifleFPMesh->SetStaticMesh(WpnMesh);
		const int32 Mats = RifleFPMesh->GetNumMaterials();
		for (int32 i = 0; i < Mats; ++i)
		{
			RifleFPMesh->SetMaterial(i, Mat);   // null reverts the slot to the mesh's authored material
		}
		RifleFPMesh->SetRelativeLocation(Def.FPLoc);
		RifleFPMesh->SetRelativeRotation(Def.FPRot);
		RifleFPMesh->SetRelativeScale3D(FVector(Def.FPScale));
	}
	MuzzleLocalFP = Def.MuzzleFP;
	ViewModelAdsLoc = Def.AdsLoc;   // per-weapon aim pose (each weapon's sight sits differently)
	ViewModelAdsRot = Def.AdsRot;

	// Per-class fire behaviour + ballistics onto the weapon component (public EditDefaultsOnly — direct write OK;
	// projectiles read MuzzleSpeedUU/ProjLifetime from here, so range flows automatically).
	if (WeaponComponent != nullptr)
	{
		WeaponComponent->SpreadHip     = Def.SpreadHipDeg;
		WeaponComponent->SpreadADS     = Def.SpreadADSDeg;
		WeaponComponent->MuzzleSpeedUU = Def.MuzzleSpeedUU;
		WeaponComponent->ProjLifetime  = Def.ProjLifetimeSec;
		WeaponComponent->BurstCount    = Def.ClassBurstCount;
		WeaponComponent->SetAllowedFireModes(Def.AllowedFireModes, Def.DefaultFireMode);
	}

	// Third-person weapon (seen by other players) — swap + re-seat on the hand.
	WeaponMesh = WpnMesh;
	AttachWeaponToHand();
}

void APaintForgeCharacter::ReapplyWeaponLoadout()
{
	ApplyWeaponLoadout();
}

void APaintForgeCharacter::SetCharSlot(int32 Slot, int32 Index)
{
	if (Slot < 0 || Slot >= PFChar::SlotCount())
	{
		return;
	}
	if (ActiveCharConfig.Slots.Num() != PFChar::SlotCount())
	{
		ActiveCharConfig = PFChar::DefaultConfig();
	}
	ActiveCharConfig.Slots[Slot] = Index;
	ApplyCharacterConfig();
}

// Browse parts without the UI (Phase 2): set a slot's part index on every character in the world.
static void PFCharSlotCmd(const TArray<FString>& Args, UWorld* World)
{
	if (World == nullptr || Args.Num() < 2)
	{
		return;
	}
	const int32 Slot = FCString::Atoi(*Args[0]);
	const int32 Index = FCString::Atoi(*Args[1]);
	int32 Applied = 0;
	for (TActorIterator<APaintForgeCharacter> It(World); It; ++It)
	{
		It->SetCharSlot(Slot, Index);
		++Applied;
	}
	UE_LOG(PaintForgeLog, Log, TEXT("pf.CharSlot: slot %d ('%s') -> part %d on %d character(s); %d parts in that slot."),
		Slot, *PFChar::SlotLabel(Slot), Index, Applied, PFChar::SlotParts(Slot).Num());
}
static FAutoConsoleCommandWithWorldAndArgs GPFCharSlotCmd(
	TEXT("pf.CharSlot"),
	TEXT("Modular character: <slotIndex> <partIndex> on all pawns. Slots: 0 Head 1 Face 2 Helmet 3 Chest 4 Arms 5 Hips 6 Pants 7 Cloth 8 Backpack (-1 part = none)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFCharSlotCmd));

void APaintForgeCharacter::TuneWeaponFP(const FVector& Loc, const FRotator& Rot, float Scale, const FVector& Muzzle)
{
	if (RifleFPMesh != nullptr)
	{
		RifleFPMesh->SetRelativeLocation(Loc);
		RifleFPMesh->SetRelativeRotation(Rot);
		RifleFPMesh->SetRelativeScale3D(FVector(Scale));
	}
	MuzzleLocalFP = Muzzle;
}

// Live-tune the equipped weapon's FP pose without a rebuild. Paste the printed values into PFWeaponCatalog.cpp.
static void PFWeaponFPCmd(const TArray<FString>& Args, UWorld* World)
{
	if (World == nullptr || Args.Num() < 7)
	{
		UE_LOG(PaintForgeLog, Log, TEXT("usage: pf.WeaponFP x y z pitch yaw roll scale [muzX muzY muzZ]"));
		return;
	}
	const FVector Loc(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]));
	const FRotator Rot(FCString::Atof(*Args[3]), FCString::Atof(*Args[4]), FCString::Atof(*Args[5]));
	const float Scale = FCString::Atof(*Args[6]);
	const FVector Muzzle = (Args.Num() >= 10)
		? FVector(FCString::Atof(*Args[7]), FCString::Atof(*Args[8]), FCString::Atof(*Args[9]))
		: FVector(42.f, 3.5f, -3.5f);
	for (TActorIterator<APaintForgeCharacter> It(World); It; ++It)
	{
		if (It->IsLocallyControlled())
		{
			It->TuneWeaponFP(Loc, Rot, Scale, Muzzle);
		}
	}
	UE_LOG(PaintForgeLog, Log,
		TEXT("pf.WeaponFP: FPLoc=FVector(%.2ff,%.2ff,%.2ff), FPRot=FRotator(%.2ff,%.2ff,%.2ff), FPScale=%.3ff, MuzzleFP=FVector(%.2ff,%.2ff,%.2ff)"),
		Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Rot.Yaw, Rot.Roll, Scale, Muzzle.X, Muzzle.Y, Muzzle.Z);
}
static FAutoConsoleCommandWithWorldAndArgs GPFWeaponFPCmd(
	TEXT("pf.WeaponFP"),
	TEXT("Tune the equipped weapon's first-person pose: x y z pitch yaw roll scale [muzX muzY muzZ]. Prints values to paste into PFWeaponCatalog."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFWeaponFPCmd));

void APaintForgeCharacter::TuneWeaponADS(const FVector& Loc, const FRotator& Rot)
{
	ViewModelAdsLoc = Loc;
	ViewModelAdsRot = Rot;
}

// Live-tune the equipped weapon's aim-down-sight pose. Hold right-click to see it; paste into PFWeaponCatalog.
static void PFWeaponADSCmd(const TArray<FString>& Args, UWorld* World)
{
	if (World == nullptr || Args.Num() < 6)
	{
		UE_LOG(PaintForgeLog, Log, TEXT("usage: pf.WeaponADS x y z pitch yaw roll  (hold right-click to preview)"));
		return;
	}
	const FVector Loc(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]));
	const FRotator Rot(FCString::Atof(*Args[3]), FCString::Atof(*Args[4]), FCString::Atof(*Args[5]));
	for (TActorIterator<APaintForgeCharacter> It(World); It; ++It)
	{
		if (It->IsLocallyControlled())
		{
			It->TuneWeaponADS(Loc, Rot);
		}
	}
	UE_LOG(PaintForgeLog, Log,
		TEXT("pf.WeaponADS: AdsLoc=FVector(%.2ff,%.2ff,%.2ff), AdsRot=FRotator(%.2ff,%.2ff,%.2ff)"),
		Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Rot.Yaw, Rot.Roll);
}
static FAutoConsoleCommandWithWorldAndArgs GPFWeaponADSCmd(
	TEXT("pf.WeaponADS"),
	TEXT("Tune the equipped weapon's aim-down-sight pose: x y z pitch yaw roll. Hold right-click to preview; prints values for PFWeaponCatalog."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PFWeaponADSCmd));

void APaintForgeCharacter::ApplyTeamBody(uint8 Team)
{
	// Mounts the per-team skeletal body. Gated so the frequent SetTeamColor calls (ready/budget/roster
	// flag replications all route here) don't re-mount the mesh every time.
	if (GetMesh() == nullptr)
	{
		return;
	}

	// The modular Bandit character is the body for every player (replaces the old Quantum/Survival team bodies).
	// Assemble once; the frequent SetTeamColor re-calls just refresh the cached team. Falls back to the legacy
	// per-team body below only if the Bandit pack is absent.
	if (!bBanditAssembled && BanditBodyMesh != nullptr)
	{
		AssembleBanditCharacter();
	}
	if (bBanditAssembled)
	{
		CachedBodyTeamId = Team;
		bUsingArtBody = true;
		return;
	}

	USkeletalMesh* Chosen = (Team == 1)
		? (Team1BodyMesh ? Team1BodyMesh : ThirdPersonBodyMesh)
		: (Team0BodyMesh ? Team0BodyMesh : ThirdPersonBodyMesh);
	if (Chosen == nullptr)
	{
		return;   // graybox fallback: keep the cubes (unconfigured character stays the validated graybox)
	}
	if (bUsingArtBody && CachedBodyTeamId == Team)
	{
		return;   // already the right body
	}

	GetMesh()->SetSkeletalMeshAsset(Chosen);

	// Anim: prefer per-team AnimBP (skeleton-guarded). Quantum has no matching ABP — use sequence
	// locomotion (idle/walk/run) instead of forcing mannequin ABP (playtest: broken pose).
	UAnimSequence* IdleAnim = (Team == 1) ? Team1IdleAnim.Get() : Team0IdleAnim.Get();
	TSubclassOf<UAnimInstance> AnimClass = (Team == 1) ? Team1AnimClass : Team0AnimClass;
	const bool bLooksLikeMannequin = Chosen->GetName().Contains(TEXT("Manny"))
		|| Chosen->GetName().Contains(TEXT("Quinn"))
		|| Chosen->GetName().Contains(TEXT("Mannequin"));
	if (AnimClass == nullptr && bLooksLikeMannequin)
	{
		AnimClass = ThirdPersonAnimClass;
	}

	// Safety: only drive with an AnimBP whose target skeleton matches this mesh.
	if (AnimClass != nullptr)
	{
		const UAnimBlueprintGeneratedClass* GenClass = Cast<UAnimBlueprintGeneratedClass>(AnimClass.Get());
		const USkeleton* AnimSkel = GenClass ? GenClass->GetTargetSkeleton() : nullptr;
		if (AnimSkel != nullptr && AnimSkel != Chosen->GetSkeleton())
		{
			UE_LOG(PaintForgeLog, Log,
				TEXT("ApplyTeamBody: AnimBP skeleton mismatch for %s — using sequence locomotion."),
				*Chosen->GetName());
			AnimClass = nullptr;
		}
	}

	GetMesh()->Stop();
	SeqLocoState = 0;
	bSequenceLocoActive = false;
	CachedWeaponAttachBone = NAME_None;
	if (AnimClass != nullptr)
	{
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		GetMesh()->SetAnimInstanceClass(AnimClass);
	}
	else
	{
		// Sequence path (Quantum): SingleNode instance + UpdateSequenceLocomotion each tick.
		GetMesh()->SetAnimInstanceClass(nullptr);
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		bSequenceLocoActive = true;
		if (IdleAnim != nullptr)
		{
			GetMesh()->PlayAnimation(IdleAnim, /*bLooping=*/true);
			SeqLocoState = 1;
			UE_LOG(PaintForgeLog, Log, TEXT("ApplyTeamBody: Quantum sequence loco ready (idle=%s)"),
				*IdleAnim->GetName());
		}
		else
		{
			UE_LOG(PaintForgeLog, Warning,
				TEXT("ApplyTeamBody: sequence loco active but no idle for %s"), *Chosen->GetName());
		}
	}

	// Align: face +X (standard ACharacter -90 yaw). Feet at capsule bottom:
	// relative Z = -halfHeight - meshMinZ (handles pelvis-origin human packs).
	GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	if (const UCapsuleComponent* Cap = GetCapsuleComponent())
	{
		const float MeshMinZ = Chosen->GetBounds().GetBox().Min.Z;
		const float FeetZ = -Cap->GetUnscaledCapsuleHalfHeight() - MeshMinZ;
		GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, FeetZ));
	}
	GetMesh()->SetVisibility(true);
	GetMesh()->SetHiddenInGame(false);
	GetMesh()->SetOwnerNoSee(true);           // first-person: owner doesn't see own TP body
	GetMesh()->SetCastShadow(true);
	GetMesh()->bCastDynamicShadow = true;
	if (BodyMesh != nullptr) { BodyMesh->SetVisibility(false); }
	if (HeadMesh != nullptr) { HeadMesh->SetVisibility(false); }

	CachedBodyTeamId = Team;
	bUsingArtBody = true;

	// Team body swap rebuilds the skeletal mesh — re-seat the rifle pose.
	AttachWeaponToHand();
}

void APaintForgeCharacter::AttachWeaponToHand()
{
	if (WeaponMeshComp == nullptr || WeaponMesh == nullptr)
	{
		return;
	}

	WeaponMeshComp->SetStaticMesh(WeaponMesh);
	WeaponMeshComp->SetVisibility(true);
	WeaponMeshComp->SetHiddenInGame(false);
	WeaponMeshComp->SetOwnerNoSee(true);   // owner uses the FP viewmodel; remotes see the TP gun
	WeaponMeshComp->SetCastShadow(true);
	{
		const int32 Mats = WeaponMeshComp->GetNumMaterials();
		for (int32 i = 0; i < Mats; ++i)
		{
			// Force our material (SM_Rifle: no missing Lyra MI refs) OR clear any stale override so a
			// preserve-authored weapon (AK/pistol) shows its own materials after a swap.
			WeaponMeshComp->SetMaterial(i, RifleMaterial);
		}
	}

	ApplyHandWeaponPose();
}

FName APaintForgeCharacter::ResolveWeaponAttachBone(const USkeletalMeshComponent* Body) const
{
	if (Body == nullptr)
	{
		return NAME_None;
	}
	// Prefer sockets first (weapon sockets), then common hand bone names (Manny + military packs).
	static const FName Candidates[] = {
		TEXT("hand_rSocket"),
		TEXT("weapon_r"),
		TEXT("WeaponPoint"),
		TEXT("hand_r"),
		TEXT("Hand_R"),
		TEXT("RightHand"),
		TEXT("ik_hand_gun"),
		TEXT("ik_hand_r"),
		TEXT("HandR"),
		TEXT("LowerArm_R"),   // last-resort closer to hand than pelvis
	};
	auto Exists = [Body](FName N) -> bool
	{
		return Body->DoesSocketExist(N) || Body->GetBoneIndex(N) != INDEX_NONE;
	};
	if (!WeaponAttachSocket.IsNone() && Exists(WeaponAttachSocket))
	{
		return WeaponAttachSocket;
	}
	for (const FName& N : Candidates)
	{
		if (Exists(N))
		{
			return N;
		}
	}
	// Scan for any bone with "hand" + "r" in the name (Quantum / Mixamo variants).
	const int32 NumBones = Body->GetNumBones();
	for (int32 i = 0; i < NumBones; ++i)
	{
		const FString Bone = Body->GetBoneName(i).ToString();
		const FString Lower = Bone.ToLower();
		// Never attach to head / neck / spine — looks like "gun on head".
		if (Lower.Contains(TEXT("head")) || Lower.Contains(TEXT("neck"))
			|| Lower.Contains(TEXT("spine")) || Lower.Contains(TEXT("clavicle")))
		{
			continue;
		}
		if ((Lower.Contains(TEXT("hand")) && (Lower.Contains(TEXT("_r")) || Lower.EndsWith(TEXT("r"))
				|| Lower.Contains(TEXT("right"))))
			|| Lower.Contains(TEXT("weapon")))
		{
			// Skip left hand.
			if (Lower.Contains(TEXT("_l")) || Lower.Contains(TEXT("left")))
			{
				continue;
			}
			return Body->GetBoneName(i);
		}
	}
	return NAME_None;
}

void APaintForgeCharacter::ApplyHandWeaponPose()
{
	if (WeaponMeshComp == nullptr || WeaponMesh == nullptr)
	{
		return;
	}

	USkeletalMeshComponent* Body = GetMesh();
	if (!bUsingArtBody || Body == nullptr)
	{
		return;
	}

	if (CachedWeaponAttachBone.IsNone())
	{
		CachedWeaponAttachBone = ResolveWeaponAttachBone(Body);
		if (!CachedWeaponAttachBone.IsNone())
		{
			UE_LOG(PaintForgeLog, Log, TEXT("Weapon attach bone: %s on %s"),
				*CachedWeaponAttachBone.ToString(),
				Body->GetSkeletalMeshAsset() ? *Body->GetSkeletalMeshAsset()->GetName() : TEXT("?"));
		}
		else
		{
			static bool bLogged = false;
			if (!bLogged)
			{
				bLogged = true;
				UE_LOG(PaintForgeLog, Warning,
					TEXT("ApplyHandWeaponPose: no hand bone on %s — hip-carry fallback. Bone dump:"),
					Body->GetSkeletalMeshAsset() ? *Body->GetSkeletalMeshAsset()->GetName() : TEXT("?"));
				const int32 NumBones = Body->GetNumBones();
				for (int32 i = 0; i < NumBones && i < 60; ++i)
				{
					UE_LOG(PaintForgeLog, Warning, TEXT("  bone[%d]=%s"), i, *Body->GetBoneName(i).ToString());
				}
			}
		}
	}

	if (!CachedWeaponAttachBone.IsNone())
	{
		if (WeaponMeshComp->GetAttachParent() != Body
			|| WeaponMeshComp->GetAttachSocketName() != CachedWeaponAttachBone)
		{
			WeaponMeshComp->AttachToComponent(Body,
				FAttachmentTransformRules::SnapToTargetNotIncludingScale, CachedWeaponAttachBone);
		}
		WeaponMeshComp->SetRelativeLocation(WeaponRelativeLocation);
		WeaponMeshComp->SetRelativeRotation(WeaponRelativeRotation);
		WeaponMeshComp->SetRelativeScale3D(WeaponRelativeScale);
		return;
	}

	// No hand bone: hip-carry in mesh space (never origin = shoulder/chest).
	WeaponMeshComp->AttachToComponent(Body,
		FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	WeaponMeshComp->SetRelativeLocation(WeaponMeshFallbackLocation);
	WeaponMeshComp->SetRelativeRotation(WeaponMeshFallbackRotation);
	WeaponMeshComp->SetRelativeScale3D(WeaponRelativeScale);
}

void APaintForgeCharacter::UpdateSequenceLocomotion()
{
	if (!bSequenceLocoActive || !bUsingArtBody || GetMesh() == nullptr)
	{
		return;
	}

	// Stay on SingleNode — if a full AnimBP was installed, leave it alone.
	if (GetMesh()->GetAnimationMode() == EAnimationMode::AnimationBlueprint
		&& GetMesh()->GetAnimInstance() != nullptr
		&& !GetMesh()->GetAnimInstance()->IsA<UAnimSingleNodeInstance>())
	{
		bSequenceLocoActive = false;
		return;
	}

	if (GetMesh()->GetAnimationMode() != EAnimationMode::AnimationSingleNode)
	{
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	}

	const bool bTeam1 = (CachedBodyTeamId == 1);
	UAnimSequence* Idle = bTeam1 ? Team1IdleAnim.Get() : Team0IdleAnim.Get();
	UAnimSequence* Walk = bTeam1 ? Team1WalkAnim.Get() : Team0WalkAnim.Get();
	UAnimSequence* Run  = bTeam1 ? Team1RunAnim.Get()  : Team0RunAnim.Get();
	if (Idle == nullptr && Walk == nullptr && Run == nullptr)
	{
		return;
	}

	// Prefer CMC velocity (replicates on proxies); actor velocity can lag for bots.
	float Speed = 0.f;
	if (const UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Speed = Move->Velocity.Size2D();
	}
	else
	{
		Speed = GetVelocity().Size2D();
	}

	// walk ~600, sprint ~830 on our CMC — pick idle / walk / run.
	uint8 Want = 1;
	if (Speed > 500.f && Run != nullptr)       { Want = 3; }
	else if (Speed > 30.f && Walk != nullptr)  { Want = 2; }
	else if (Speed > 30.f && Run != nullptr)   { Want = 3; }
	else                                       { Want = 1; }

	if (Want == SeqLocoState)
	{
		return;
	}
	SeqLocoState = Want;

	UAnimSequence* Seq = Idle;
	if (Want == 2) { Seq = Walk ? Walk : (Run ? Run : Idle); }
	else if (Want == 3) { Seq = Run ? Run : (Walk ? Walk : Idle); }
	if (Seq == nullptr)
	{
		return;
	}

	// Use SingleNode API — more reliable than PlayAnimation when mode was already set.
	GetMesh()->PlayAnimation(Seq, /*bLooping=*/true);
	if (UAnimSingleNodeInstance* Node = GetMesh()->GetSingleNodeInstance())
	{
		Node->SetLooping(true);
		Node->SetPlaying(true);
		Node->SetPlayRate(1.f);
	}
}

void APaintForgeCharacter::UpdateBuildPhaseWeaponVisibility()
{
	bool bHideForBuild = false;
	if (const UWorld* World = GetWorld())
	{
		if (const APaintForgeGameState* GS = World->GetGameState<APaintForgeGameState>())
		{
			bHideForBuild = (GS->Phase == EPFMatchPhase::Build);
		}
	}

	// FP viewmodel (owner): fully off during build so it doesn't block placement.
	if (ViewModelRoot != nullptr)
	{
		ViewModelRoot->SetVisibility(!bHideForBuild, /*bPropagateToChildren=*/true);
	}
	// TP rifle (remotes): also hide so builders don't look armed.
	if (WeaponMeshComp != nullptr)
	{
		// Don't fight elimination hide — elim appearance owns that path.
		if (bHideForBuild)
		{
			WeaponMeshComp->SetHiddenInGame(true);
		}
		// When leaving build, re-show unless eliminated (SetEliminatedAppearance may re-hide).
		else if (const UPFHealthComponent* Health = GetHealth())
		{
			WeaponMeshComp->SetHiddenInGame(Health->bEliminated);
		}
		else
		{
			WeaponMeshComp->SetHiddenInGame(false);
		}
	}
}

bool APaintForgeCharacter::ShouldRaiseWeapon() const
{
	// Raise whenever we're aiming or shooting so the muzzle leaves hip height.
	if (IsADS())
	{
		return true;
	}
	if (bFireHeld)
	{
		return true;
	}
	if (WeaponComponent != nullptr && WeaponComponent->WantsFire())
	{
		return true;
	}
	if (WeaponRaiseHoldSec > 0.f)
	{
		return true;
	}
	return false;
}

FVector APaintForgeCharacter::GetEyeWorldLocation() const
{
	if (const UCapsuleComponent* Cap = GetCapsuleComponent())
	{
		const float EyeUp = Cap->GetScaledCapsuleHalfHeight() - CameraEyeOffsetFromCapsuleTop;
		return GetActorLocation() + Cap->GetUpVector() * EyeUp;
	}
	return GetActorLocation() + FVector(0.f, 0.f, 60.f);
}

void APaintForgeCharacter::ApplyRaisedWeaponPose()
{
	if (WeaponMeshComp == nullptr || WeaponMesh == nullptr)
	{
		return;
	}

	const FRotator Aim = GetBaseAimRotation();
	const FRotationMatrix AimM(Aim);
	// Place mesh origin so the barrel sits on the aim line at eye height (not hip).
	// ADS pulls the TP raise slightly closer for cleaner silhouette.
	const float Ads = FMath::Clamp(GetADSAlpha(), 0.f, 1.f);
	const FVector Raised = FMath::Lerp(WeaponRaisedFromEye, FVector(22.f, 10.f, -6.f), Ads);
	const FVector Origin = GetEyeWorldLocation()
		+ AimM.GetUnitAxis(EAxis::X) * Raised.X
		+ AimM.GetUnitAxis(EAxis::Y) * Raised.Y
		+ AimM.GetUnitAxis(EAxis::Z) * Raised.Z;

	if (USceneComponent* Root = GetRootComponent())
	{
		if (WeaponMeshComp->GetAttachParent() != Root)
		{
			WeaponMeshComp->AttachToComponent(Root,
				FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		}
	}
	WeaponMeshComp->SetWorldLocation(Origin);
	// SM_Rifle / olive: local +Y is barrel-forward → yaw -90 into aim +X.
	WeaponMeshComp->SetWorldRotation(FRotator(Aim.Pitch, Aim.Yaw - 90.f, 0.f));
	WeaponMeshComp->SetWorldScale3D(WeaponRelativeScale);
}

void APaintForgeCharacter::UpdateWeaponHoldPose()
{
	if (WeaponMeshComp == nullptr || WeaponMesh == nullptr || !bUsingArtBody)
	{
		return;
	}
	if (WeaponMeshComp->bHiddenInGame)
	{
		return;
	}
	// Always keep the TP gun on the hand bone. The old "raise to eye" path parented the
	// mesh to the capsule root at eye height — remotes saw every gun sitting on the head
	// and auth balls spawned from that tip. FP viewmodel (camera) stays separate for the owner.
	ApplyHandWeaponPose();
}

void APaintForgeCharacter::ApplyArtLoadout()
{
	// Optional M1 art. Every branch is a no-op when its property is unset, so an
	// unconfigured character stays byte-identical to the validated graybox.
	bUsingArtBody = false;
	CachedBodyTeamId = 255;

	// Provisional body from the (possibly not-yet-replicated) team; SetTeamColor re-drives it
	// authoritatively when the real TeamId lands.
	ApplyTeamBody(CachedTeamId);

	if (FirstPersonArmsMesh != nullptr && FirstPersonArms != nullptr)
	{
		FirstPersonArms->SetSkeletalMeshAsset(FirstPersonArmsMesh);
		FirstPersonArms->SetVisibility(true);
	}

	AttachWeaponToHand();

	SetTeamColor(CachedTeamId);   // re-tint whatever body is now active (skeletal or cubes)
}

void APaintForgeCharacter::SetTeamColor(uint8 TeamId)
{
	// SetTeamColor is the one convergence point that runs on server, host, and pure clients once the real
	// TeamId is known (in either replication order) and again on host team-cycle — so the per-team BODY
	// swap lives here, not in BeginPlay (where a client often doesn't know its team yet).
	const bool bTeamChanged = (TeamId != CachedTeamId) || !bTeamAppearanceApplied;
	CachedTeamId = TeamId;

	if (TeamId <= 1)
	{
		ApplyTeamBody(TeamId);   // gated: re-mounts Manny/Quinn only on an actual team change
	}

	if (!bTeamChanged)
	{
		return;   // appearance already applied for this team; don't rebuild MIDs on every flag replication
	}
	bTeamAppearanceApplied = true;

	const FLinearColor TeamColor = PFColors::ForTeam(TeamId);
	// Team armband takes the full vivid team color (max readability — it's the primary friend/foe tell).
	if (ArmbandMID != nullptr)
	{
		ArmbandMID->SetVectorParameterValue(TEXT("Color"), TeamColor);
	}
	if (ArmbandMIDR != nullptr)
	{
		ArmbandMIDR->SetVectorParameterValue(TEXT("Color"), TeamColor);
	}
	// Muted team tint for the soldier body — clearly team-colored but not neon speedball (splats/tracers
	// keep the full vivid ForTeam color). Tune the 0.45 lerp toward gray to taste.
	const FLinearColor BodyTint = FMath::Lerp(TeamColor, FLinearColor(0.22f, 0.22f, 0.24f, 1.f), 0.45f);

	if (BodyMesh != nullptr && BodyMID == nullptr)
	{
		BodyMID = BodyMesh->CreateAndSetMaterialInstanceDynamic(0);
	}
	if (HeadMesh != nullptr && HeadMID == nullptr)
	{
		HeadMID = HeadMesh->CreateAndSetMaterialInstanceDynamic(0);
	}
	if (BodyMID != nullptr)
	{
		BodyMID->SetVectorParameterValue(TEXT("Color"), TeamColor);
	}
	if (HeadMID != nullptr)
	{
		HeadMID->SetVectorParameterValue(TEXT("Color"), TeamColor);
	}

	// Human packs (Quantum / Survival) keep their multi-slot authored materials — team identity
	// is the model itself. Mannequin/graybox may still take a single-slot tint override.
	if (bUsingArtBody && GetMesh() != nullptr && !bPreserveAuthoredMaterials)
	{
		UMaterialInterface* NativeMat = (TeamId == 1)
			? (Team1BodyMaterial ? Team1BodyMaterial.Get() : Team0BodyMaterial.Get())
			: Team0BodyMaterial.Get();

		if (NativeMat != nullptr)
		{
			const int32 NumMats = GetMesh()->GetNumMaterials();
			for (int32 Slot = 0; Slot < NumMats; ++Slot)
			{
				GetMesh()->SetMaterial(Slot, NativeMat);
			}
		}
		else if (TeamBodyMaterial != nullptr)
		{
			const int32 NumMats = GetMesh()->GetNumMaterials();
			for (int32 Slot = 0; Slot < NumMats; ++Slot)
			{
				if (UMaterialInstanceDynamic* BodyArtMID =
						GetMesh()->CreateAndSetMaterialInstanceDynamicFromMaterial(Slot, TeamBodyMaterial))
				{
					BodyArtMID->SetVectorParameterValue(TEXT("Color"), BodyTint);
				}
			}
		}
	}
}

void APaintForgeCharacter::SetEliminatedAppearance(bool bEliminated)
{
	// Mesh only — collision timing (0.5 s corpse block) is the health
	// component's job (04 §2.4).
	if (BodyMesh != nullptr)
	{
		BodyMesh->SetHiddenInGame(bEliminated);
	}
	if (HeadMesh != nullptr)
	{
		HeadMesh->SetHiddenInGame(bEliminated);
	}
	if (bUsingArtBody && GetMesh() != nullptr)
	{
		// Propagate to children so the modular character parts (CharBaseComps/CharSlotComps) + armband hide with
		// the base body — otherwise an eliminated pawn leaves its clothing/head/legs frozen in the idle pose.
		GetMesh()->SetHiddenInGame(bEliminated, /*bPropagateToChildren=*/true);
	}
	if (WeaponMeshComp != nullptr)
	{
		WeaponMeshComp->SetHiddenInGame(bEliminated);
	}
	if (ViewModelRoot != nullptr)
	{
		ViewModelRoot->SetVisibility(!bEliminated, /*bPropagateToChildren=*/true);
	}
}

FVector APaintForgeCharacter::GetMuzzleLocation(bool bCosmetic) const
{
	// Owning-client cosmetic tracers: FP barrel tip (viewmodel on camera — correct FPS height).
	if (bCosmetic && ViewModelRoot != nullptr && !MuzzleLocalFP.IsNearlyZero())
	{
		return ViewModelRoot->GetComponentTransform().TransformPosition(MuzzleLocalFP);
	}
	if (bCosmetic && FirstPersonCamera != nullptr)
	{
		return FirstPersonCamera->GetComponentLocation() + FirstPersonCamera->GetForwardVector() * 55.f;
	}

	// Authoritative / remote: spawn from the TP rifle barrel when it's seated on the body.
	if (WeaponMeshComp != nullptr && WeaponMeshComp->GetStaticMesh() != nullptr && bUsingArtBody
		&& !WeaponMeshComp->bHiddenInGame
		&& WeaponMeshComp->GetAttachParent() == GetMesh())
	{
		return WeaponMeshComp->GetComponentTransform().TransformPosition(RifleMuzzleLocalTP);
	}

	// Fallback: slightly forward of the eye (no art body / hidden gun).
	const FRotationMatrix AimBasis(GetBaseAimRotation());
	return GetEyeWorldLocation()
		+ AimBasis.GetUnitAxis(EAxis::X) * 55.f
		+ AimBasis.GetUnitAxis(EAxis::Y) * 12.f
		+ AimBasis.GetUnitAxis(EAxis::Z) * (-8.f);
}

// ---------------------------------------------------------------------------
// Weapon viewmodel + muzzle-flash cosmetics (M1 art pass)
// ---------------------------------------------------------------------------

UStaticMeshComponent* APaintForgeCharacter::MakeGunPart(USceneComponent* Parent, const FString& CompName,
	UStaticMesh* PartMesh, UMaterialInterface* Mat, const FVector& RelLoc, const FVector& RelScale,
	const FRotator& RelRot)
{
	UStaticMeshComponent* Part = CreateDefaultSubobject<UStaticMeshComponent>(FName(*CompName));
	if (Part != nullptr)
	{
		Part->SetupAttachment(Parent);
		if (PartMesh != nullptr) { Part->SetStaticMesh(PartMesh); }
		if (Mat != nullptr) { Part->SetMaterial(0, Mat); }
		Part->SetRelativeLocation(RelLoc);
		Part->SetRelativeScale3D(RelScale);
		Part->SetRelativeRotation(RelRot);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCastShadow(false);
	}
	return Part;
}

void APaintForgeCharacter::BuildMarker(USceneComponent* Parent, const FString& Prefix, UStaticMesh* Cube,
	UStaticMesh* Cylinder, TArray<TObjectPtr<UStaticMeshComponent>>& OutParts, FVector& OutMuzzleLocal)
{
	// Silhouette in uu (barrel points +X). Engine Cube/Cylinder are 100 uu, so scale = size/100.
	// Base material is assigned by the caller (owner-only-see for the FP viewmodel).
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Recv"), Cube, nullptr,      // receiver / upper
		FVector(0.f, 0.f, 0.f), FVector(0.34f, 0.06f, 0.10f), FRotator::ZeroRotator));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Guard"), Cube, nullptr,     // handguard
		FVector(24.f, 0.f, -1.f), FVector(0.22f, 0.05f, 0.055f), FRotator::ZeroRotator));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Barrel"), Cylinder, nullptr, // barrel (cyl +Z -> +X)
		FVector(40.f, 0.f, 1.5f), FVector(0.024f, 0.024f, 0.28f), FRotator(90.f, 0.f, 0.f)));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Stock"), Cube, nullptr,     // stock
		FVector(-20.f, 0.f, -1.5f), FVector(0.20f, 0.05f, 0.075f), FRotator::ZeroRotator));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Mag"), Cube, nullptr,       // magazine (angled)
		FVector(6.f, 0.f, -11.f), FVector(0.05f, 0.035f, 0.16f), FRotator(0.f, 0.f, 18.f)));
	OutParts.Add(MakeGunPart(Parent, Prefix + TEXT("Grip"), Cube, nullptr,      // pistol grip
		FVector(-6.f, 0.f, -9.f), FVector(0.045f, 0.045f, 0.12f), FRotator(0.f, 0.f, -12.f)));
	OutMuzzleLocal = FVector(54.f, 0.f, 1.5f);   // barrel tip
}

void APaintForgeCharacter::SetupWeaponMaterials()
{
	// Dark gunmetal on the primitive marker fallback (real SM_Rifle uses M_PF_Rifle).
	if (MarkerPartsFP.Num() > 0 && MarkerPartsFP[0] != nullptr)
	{
		MarkerMID = MarkerPartsFP[0]->CreateAndSetMaterialInstanceDynamic(0);
		if (MarkerMID != nullptr)
		{
			MarkerMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.035f, 0.038f, 0.045f));
			for (int32 Index = 1; Index < MarkerPartsFP.Num(); ++Index)
			{
				if (MarkerPartsFP[Index] != nullptr)
				{
					MarkerPartsFP[Index]->SetMaterial(0, MarkerMID);
				}
			}
		}
	}
}

void APaintForgeCharacter::OnFireCosmetic(float RecoilScale)
{
	// Airsoft marker: viewmodel recoil only — no muzzle flash. Kick scaled by ADS + mag-ramp (see FireOneShot).
	RecoilOffset += FVector(-RecoilKickUU, 0.f, RecoilKickUU * 0.35f) * RecoilScale;
	RecoilPitch += RecoilKickPitchDeg * RecoilScale;
	// Keep TP gun raised through the shot cadence so muzzle/balls aren't hip-height.
	WeaponRaiseHoldSec = FMath::Max(WeaponRaiseHoldSec, WeaponRaiseHoldOnShot);
	if (bUsingArtBody)
	{
		ApplyRaisedWeaponPose();
	}
}

void APaintForgeCharacter::OnRemoteFireCosmetic()
{
	// Airsoft: no flash. Brief TP raise so remotes see the marker up with the shot.
	WeaponRaiseHoldSec = FMath::Max(WeaponRaiseHoldSec, WeaponRaiseHoldOnShot);
	if (bUsingArtBody)
	{
		ApplyRaisedWeaponPose();
	}
}
