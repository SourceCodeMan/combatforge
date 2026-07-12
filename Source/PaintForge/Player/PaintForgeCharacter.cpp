// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PaintForgeCharacter.h"

#include "PaintForge.h"
#include "Core/PaintForgeTypes.h"
#include "Core/PaintForgePlayerController.h"
#include "Player/PFCharacterMovementComponent.h"
#include "Player/PFCameraShakes.h"
#include "Input/PFInputConfig.h"
#include "Combat/PFWeaponComponent.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFCombatAudio.h"
#include "Building/PFBuildComponent.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimInstance.h"
#include "EnhancedInputComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
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
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> RifleMeshFinder(TEXT("/Game/Weapons/Rifle/Mesh/SM_Rifle.SM_Rifle"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RifleMatFinder(TEXT("/Game/Weapons/Rifle/M_PF_Rifle.M_PF_Rifle"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FlashMatFinder(
		TEXT("/Game/Materials/M_PF_Flash.M_PF_Flash"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SmokeMatFinder(
		TEXT("/Game/Materials/M_PF_MuzzleSmoke.M_PF_MuzzleSmoke"));
	FlashMaterial = FlashMatFinder.Succeeded() ? FlashMatFinder.Object.Get()
		: (MatFinder.Succeeded() ? MatFinder.Object.Get() : nullptr);
	SmokeMaterial = SmokeMatFinder.Succeeded() ? SmokeMatFinder.Object.Get() : FlashMaterial.Get();
	UStaticMesh* CubeMesh = CubeFinder.Succeeded() ? CubeFinder.Object : nullptr;
	UStaticMesh* CylMesh = CylFinder.Succeeded() ? CylFinder.Object.Get() : CubeMesh;
	UMaterialInterface* GunBaseMat = MatFinder.Succeeded() ? MatFinder.Object : nullptr;

	ViewModelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ViewModelRoot"));
	ViewModelRoot->SetupAttachment(FirstPersonCamera);
	ViewModelHomeLoc = FVector(28.f, 10.f, -13.f);   // forward-right-down of the eye, classic viewmodel pose
	ViewModelRoot->SetRelativeLocation(ViewModelHomeLoc);

	// First-person weapon: the real rifle if available (Lyra SM_Rifle + generated M_PF_Rifle, self-contained),
	// otherwise the primitive marker gun. We build ONE or the OTHER — building no primitive means the
	// eliminate/respawn visibility propagate (SetChildVisibility) can never re-show an old primitive gun.
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
		// Held-rifle pose in ViewModelRoot space (+X forward, +Y right, +Z up). The mesh models forward along
		// its local +Y, so yaw -90 points the barrel into the screen; slightly larger + lower for "in hands" feel.
		RifleFPMesh->SetRelativeLocation(FVector(4.f, 6.f, -4.f));
		RifleFPMesh->SetRelativeRotation(FRotator(-2.f, -90.f, 2.f));
		RifleFPMesh->SetRelativeScale3D(FVector(0.45f));
		MuzzleLocalFP = FVector(38.f, 3.f, -5.f);   // rifle barrel tip in ViewModelRoot space (drives the FP flash)
		WeaponMesh = RifleMesh;                      // third-person seam (attaches to the hand bone in ApplyArtLoadout)
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

	// FP muzzle flash core (owner) — hot emissive sphere at the barrel tip.
	MuzzleFlashFP = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MuzzleFlashFP"));
	MuzzleFlashFP->SetupAttachment(ViewModelRoot);
	if (SphereFinder.Succeeded()) { MuzzleFlashFP->SetStaticMesh(SphereFinder.Object); }
	if (FlashMaterial != nullptr) { MuzzleFlashFP->SetMaterial(0, FlashMaterial); }
	MuzzleFlashFP->SetRelativeLocation(MuzzleLocalFP);
	MuzzleFlashFP->SetRelativeScale3D(FVector(0.14f));
	MuzzleFlashFP->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MuzzleFlashFP->SetCastShadow(false);
	MuzzleFlashFP->SetOnlyOwnerSee(true);
	MuzzleFlashFP->SetVisibility(false);

	// FP muzzle smoke wisp — short residual gray puff just past the flash (airsoft, not paint spray).
	MuzzleSmokeFP = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MuzzleSmokeFP"));
	MuzzleSmokeFP->SetupAttachment(ViewModelRoot);
	if (SphereFinder.Succeeded()) { MuzzleSmokeFP->SetStaticMesh(SphereFinder.Object); }
	if (SmokeMaterial != nullptr) { MuzzleSmokeFP->SetMaterial(0, SmokeMaterial); }
	MuzzleSmokeFP->SetRelativeLocation(MuzzleLocalFP + FVector(6.f, 0.f, 0.f));
	MuzzleSmokeFP->SetRelativeScale3D(FVector(0.18f, 0.10f, 0.10f));
	MuzzleSmokeFP->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MuzzleSmokeFP->SetCastShadow(false);
	MuzzleSmokeFP->SetOnlyOwnerSee(true);
	MuzzleSmokeFP->SetVisibility(false);

	// TP muzzle flash core (viewers) — world-placed at the shooter's marker each shot.
	MuzzleFlashTP = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MuzzleFlashTP"));
	MuzzleFlashTP->SetupAttachment(Capsule);
	MuzzleFlashTP->SetUsingAbsoluteLocation(true);
	if (SphereFinder.Succeeded()) { MuzzleFlashTP->SetStaticMesh(SphereFinder.Object); }
	if (FlashMaterial != nullptr) { MuzzleFlashTP->SetMaterial(0, FlashMaterial); }
	MuzzleFlashTP->SetRelativeScale3D(FVector(0.22f));
	MuzzleFlashTP->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MuzzleFlashTP->SetCastShadow(false);
	MuzzleFlashTP->SetOwnerNoSee(true);
	MuzzleFlashTP->SetVisibility(false);

	MuzzleSmokeTP = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MuzzleSmokeTP"));
	MuzzleSmokeTP->SetupAttachment(Capsule);
	MuzzleSmokeTP->SetUsingAbsoluteLocation(true);
	if (SphereFinder.Succeeded()) { MuzzleSmokeTP->SetStaticMesh(SphereFinder.Object); }
	if (SmokeMaterial != nullptr) { MuzzleSmokeTP->SetMaterial(0, SmokeMaterial); }
	MuzzleSmokeTP->SetRelativeScale3D(FVector(0.28f, 0.16f, 0.16f));
	MuzzleSmokeTP->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MuzzleSmokeTP->SetCastShadow(false);
	MuzzleSmokeTP->SetOwnerNoSee(true);
	MuzzleSmokeTP->SetVisibility(false);

	// Shared muzzle light — tight bright pulse, no shadows (perf / multiplayer-friendly).
	MuzzleLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("MuzzleLight"));
	MuzzleLight->SetupAttachment(Capsule);
	MuzzleLight->SetUsingAbsoluteLocation(true);
	MuzzleLight->SetIntensity(0.f);
	MuzzleLight->SetAttenuationRadius(MuzzleLightRadius);
	MuzzleLight->SetLightColor(FLinearColor(1.0f, 0.70f, 0.32f));
	MuzzleLight->SetCastShadows(false);
	MuzzleLight->SetSpecularScale(0.2f);

	// M1: default the art body to the imported UE Mannequin (Third Person content pack) so the
	// graybox cubes become a real animated humanoid. .Succeeded() guards keep the graybox fallback
	// if the pack isn't present. SKM_Manny_Simple is the non-Nanite variant (renders on SM5).
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MannequinBodyFinder(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> QuinnBodyFinder(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple.SKM_Quinn_Simple"));
	if (MannequinBodyFinder.Succeeded())
	{
		ThirdPersonBodyMesh = MannequinBodyFinder.Object;   // fallback body (also team 0)
		Team0BodyMesh = MannequinBodyFinder.Object;         // team 0 = Manny
	}
	// Team 1 = Quinn if present, else fall back to Manny (still tinted distinctly).
	Team1BodyMesh = QuinnBodyFinder.Succeeded() ? QuinnBodyFinder.Object.Get() : ThirdPersonBodyMesh.Get();
	static ConstructorHelpers::FClassFinder<UAnimInstance> MannequinAnimFinder(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"));
	if (MannequinAnimFinder.Succeeded())
	{
		ThirdPersonAnimClass = MannequinAnimFinder.Class;   // same skeleton -> one anim BP for both teams
	}
	// Soft team-tint fallback (used only when native mannequin MIs are missing).
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TeamBodyMatFinder(
		TEXT("/Game/Materials/M_PF_TeamBody.M_PF_TeamBody"));
	if (TeamBodyMatFinder.Succeeded())
	{
		TeamBodyMaterial = TeamBodyMatFinder.Object;
	}
	// Prefer native mannequin materials so kids see textured Manny/Quinn, not a flat team wash.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MannyMatFinder(
		TEXT("/Game/Characters/Mannequins/Materials/Manny/MI_Manny_01_New.MI_Manny_01_New"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> QuinnMatFinder(
		TEXT("/Game/Characters/Mannequins/Materials/Quinn/MI_Quinn_01.MI_Quinn_01"));
	if (MannyMatFinder.Succeeded())
	{
		Team0BodyMaterial = MannyMatFinder.Object;
	}
	if (QuinnMatFinder.Succeeded())
	{
		Team1BodyMaterial = QuinnMatFinder.Object;
	}
	// TP weapon: same SM_Rifle as the FP viewmodel so remote players see a held gun (not a cube).
	if (RifleMeshFinder.Succeeded())
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
}

void APaintForgeCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PFMovement != nullptr)
	{
		PFMovement->OnSlideStateChanged.RemoveAll(this);
	}
	GetWorldTimerManager().ClearTimer(SprintOutTimerHandle);
	GetWorldTimerManager().ClearTimer(BufferedJumpClearHandle);
	GetWorldTimerManager().ClearTimer(MuzzleFlashTimerHandle);
	GetWorldTimerManager().ClearTimer(MuzzleSmokeTimerHandle);

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

		// Viewmodel recoil recovery: the per-shot kick springs back toward the resting pose.
		if (ViewModelRoot != nullptr)
		{
			RecoilOffset = FMath::VInterpTo(RecoilOffset, FVector::ZeroVector, DeltaSeconds, RecoilRecoverSpeed);
			RecoilPitch = FMath::FInterpTo(RecoilPitch, 0.f, DeltaSeconds, RecoilRecoverSpeed);
			ViewModelRoot->SetRelativeLocation(ViewModelHomeLoc + RecoilOffset);
			ViewModelRoot->SetRelativeRotation(FRotator(RecoilPitch, 0.f, 0.f));
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

void APaintForgeCharacter::ApplyTeamBody(uint8 Team)
{
	// Mounts the per-team skeletal body. Gated so the frequent SetTeamColor calls (ready/budget/roster
	// flag replications all route here) don't re-mount the mesh every time.
	if (GetMesh() == nullptr)
	{
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
	if (ThirdPersonAnimClass != nullptr)
	{
		GetMesh()->SetAnimInstanceClass(ThirdPersonAnimClass);
	}
	// Align: face +X (standard ACharacter -90 yaw), feet at the capsule bottom. Use the FEET-AT-ORIGIN
	// convention (-halfHeight, matching the ctor smoothing baseline). Log only — ensureMsgf can
	// freeze/assert on Development client builds and looks like a mid-match "crash" to kids.
	GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	if (const UCapsuleComponent* Cap = GetCapsuleComponent())
	{
		const float MeshMinZ = Chosen->GetBounds().GetBox().Min.Z;
		if (!FMath::IsNearlyZero(MeshMinZ, 2.f))
		{
			UE_LOG(PaintForgeLog, Warning,
				TEXT("Team body %s isn't feet-at-origin (minZ=%.1f); may float on remote screens."),
				*Chosen->GetName(), MeshMinZ);
		}
		GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, -Cap->GetUnscaledCapsuleHalfHeight()));
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

	// Team body swap rebuilds the skeletal mesh — re-seat the rifle in the hand.
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
	if (RifleMaterial != nullptr)
	{
		const int32 Mats = WeaponMeshComp->GetNumMaterials();
		for (int32 i = 0; i < Mats; ++i)
		{
			WeaponMeshComp->SetMaterial(i, RifleMaterial);   // no missing Lyra MI refs
		}
	}

	USkeletalMeshComponent* Body = GetMesh();
	if (!bUsingArtBody || Body == nullptr)
	{
		return;
	}

	// DoesSocketExist only lists authored sockets; UE5 mannequin hand is a BONE (hand_r).
	const bool bHasSocket = Body->DoesSocketExist(WeaponAttachSocket);
	const bool bHasBone = Body->GetBoneIndex(WeaponAttachSocket) != INDEX_NONE;
	if (!bHasSocket && !bHasBone)
	{
		UE_LOG(PaintForgeLog, Warning,
			TEXT("AttachWeaponToHand: no socket/bone '%s' on %s — gun stays on mesh root."),
			*WeaponAttachSocket.ToString(),
			Body->GetSkeletalMeshAsset() ? *Body->GetSkeletalMeshAsset()->GetName() : TEXT("(none)"));
		return;
	}

	WeaponMeshComp->AttachToComponent(Body,
		FAttachmentTransformRules::SnapToTargetNotIncludingScale, WeaponAttachSocket);
	WeaponMeshComp->SetRelativeLocation(WeaponRelativeLocation);
	WeaponMeshComp->SetRelativeRotation(WeaponRelativeRotation);
	WeaponMeshComp->SetRelativeScale3D(WeaponRelativeScale);
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

	// Art body: prefer native Manny/Quinn materials (textured characters kids can read as "people").
	// Fall back to M_PF_TeamBody soft tint only when the pack MIs are missing.
	if (bUsingArtBody && GetMesh() != nullptr)
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
		GetMesh()->SetHiddenInGame(bEliminated);
	}
	if (WeaponMeshComp != nullptr)
	{
		WeaponMeshComp->SetHiddenInGame(bEliminated);
	}
	if (ViewModelRoot != nullptr)
	{
		ViewModelRoot->SetVisibility(!bEliminated, /*bPropagateToChildren=*/true);
		// The propagate un-hides EVERY child — re-hide the muzzle FX, which must only flash during a shot.
		if (MuzzleFlashFP != nullptr) { MuzzleFlashFP->SetVisibility(false); }
		if (MuzzleSmokeFP != nullptr) { MuzzleSmokeFP->SetVisibility(false); }
	}
}

FVector APaintForgeCharacter::GetMuzzleLocation(bool bCosmetic) const
{
	if (bCosmetic && FirstPersonCamera != nullptr)
	{
		// Owning-client cosmetic: camera + 20 forward — never clips own geometry.
		return FirstPersonCamera->GetComponentLocation() + FirstPersonCamera->GetForwardVector() * 20.f;
	}

	// Server / sim proxies: capsule center + 30 fwd + 20 right - 10 down along
	// the aim basis (04 §2.2).
	const FRotationMatrix AimBasis(GetBaseAimRotation());
	return GetActorLocation()
		+ AimBasis.GetUnitAxis(EAxis::X) * 30.f
		+ AimBasis.GetUnitAxis(EAxis::Y) * 20.f
		- AimBasis.GetUnitAxis(EAxis::Z) * 10.f;
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
	// Dark gunmetal on the marker: one MID shared across all parts (they all wrap BasicShapeMaterial).
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

	// Emissive flash + smoke. Param names cover M_PF_Flash / M_PF_MuzzleSmoke and BasicShapeMaterial.
	const FLinearColor FlashColor(8.f, 4.2f, 1.2f);
	const FLinearColor SmokeColor(0.55f, 0.58f, 0.62f);
	auto SetupEmissiveMID = [](UStaticMeshComponent* Comp, const FLinearColor& Color, float Strength)
		-> UMaterialInstanceDynamic*
	{
		if (Comp == nullptr) { return nullptr; }
		UMaterialInstanceDynamic* MID = Comp->CreateAndSetMaterialInstanceDynamic(0);
		if (MID != nullptr)
		{
			MID->SetVectorParameterValue(TEXT("EmissiveColor"), Color);
			MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Strength);
			MID->SetVectorParameterValue(TEXT("Color"), Color);
		}
		return MID;
	};
	FlashMIDFP = SetupEmissiveMID(MuzzleFlashFP, FlashColor, 1.6f);
	FlashMIDTP = SetupEmissiveMID(MuzzleFlashTP, FlashColor, 1.6f);
	SmokeMIDFP = SetupEmissiveMID(MuzzleSmokeFP, SmokeColor, 0.5f);
	SmokeMIDTP = SetupEmissiveMID(MuzzleSmokeTP, SmokeColor, 0.5f);
}

void APaintForgeCharacter::OnFireCosmetic()
{
	// Owning client: recoil kick + punchy FP flash + smoke wisp + tight world light.
	RecoilOffset += FVector(-RecoilKickUU, 0.f, RecoilKickUU * 0.35f);
	RecoilPitch += RecoilKickPitchDeg;

	const float FlashS = FMath::FRandRange(0.12f, 0.20f);
	// Slightly flattened "petal" flash with random roll so frames don't look identical.
	if (MuzzleFlashFP != nullptr)
	{
		MuzzleFlashFP->SetRelativeScale3D(FVector(FlashS * 0.7f, FlashS * 1.15f, FlashS * 1.15f));
		MuzzleFlashFP->SetRelativeRotation(FRotator(0.f, 0.f, FMath::FRandRange(0.f, 360.f)));
		MuzzleFlashFP->SetVisibility(true);
	}
	if (MuzzleSmokeFP != nullptr)
	{
		const float SmokeS = FMath::FRandRange(0.16f, 0.24f);
		MuzzleSmokeFP->SetRelativeScale3D(FVector(SmokeS * 1.6f, SmokeS * 0.85f, SmokeS * 0.85f));
		MuzzleSmokeFP->SetVisibility(true);
	}
	if (MuzzleLight != nullptr)
	{
		MuzzleLight->SetWorldLocation(GetMuzzleLocation(/*bCosmetic=*/true));
		MuzzleLight->SetAttenuationRadius(MuzzleLightRadius);
		MuzzleLight->SetIntensity(MuzzleLightIntensity * FMath::FRandRange(0.85f, 1.1f));
	}
	GetWorldTimerManager().SetTimer(MuzzleFlashTimerHandle, this,
		&APaintForgeCharacter::ClearMuzzleFlash, MuzzleFlashTime, false);
	GetWorldTimerManager().SetTimer(MuzzleSmokeTimerHandle, this,
		&APaintForgeCharacter::ClearMuzzleSmoke, MuzzleSmokeTime, false);
}

void APaintForgeCharacter::OnRemoteFireCosmetic()
{
	// Remote viewers: flash + smoke + light at the shooter's marker position.
	const FVector Muzzle = GetMuzzleLocation(/*bCosmetic=*/false);
	const FVector AimFwd = GetBaseAimRotation().Vector();
	if (MuzzleFlashTP != nullptr)
	{
		const float FlashS = FMath::FRandRange(0.18f, 0.28f);
		MuzzleFlashTP->SetWorldLocation(Muzzle);
		MuzzleFlashTP->SetWorldScale3D(FVector(FlashS * 0.7f, FlashS * 1.15f, FlashS * 1.15f));
		MuzzleFlashTP->SetVisibility(true);
	}
	if (MuzzleSmokeTP != nullptr)
	{
		const float SmokeS = FMath::FRandRange(0.22f, 0.32f);
		MuzzleSmokeTP->SetWorldLocation(Muzzle + AimFwd * 12.f);
		MuzzleSmokeTP->SetWorldScale3D(FVector(SmokeS * 1.5f, SmokeS * 0.9f, SmokeS * 0.9f));
		MuzzleSmokeTP->SetVisibility(true);
	}
	if (MuzzleLight != nullptr)
	{
		MuzzleLight->SetWorldLocation(Muzzle);
		MuzzleLight->SetAttenuationRadius(MuzzleLightRadius);
		MuzzleLight->SetIntensity(MuzzleLightIntensity * FMath::FRandRange(0.85f, 1.1f));
	}
	GetWorldTimerManager().SetTimer(MuzzleFlashTimerHandle, this,
		&APaintForgeCharacter::ClearMuzzleFlash, MuzzleFlashTime, false);
	GetWorldTimerManager().SetTimer(MuzzleSmokeTimerHandle, this,
		&APaintForgeCharacter::ClearMuzzleSmoke, MuzzleSmokeTime, false);
}

void APaintForgeCharacter::ClearMuzzleFlash()
{
	if (MuzzleFlashFP != nullptr) { MuzzleFlashFP->SetVisibility(false); }
	if (MuzzleFlashTP != nullptr) { MuzzleFlashTP->SetVisibility(false); }
	if (MuzzleLight != nullptr) { MuzzleLight->SetIntensity(0.f); }
}

void APaintForgeCharacter::ClearMuzzleSmoke()
{
	if (MuzzleSmokeFP != nullptr) { MuzzleSmokeFP->SetVisibility(false); }
	if (MuzzleSmokeTP != nullptr) { MuzzleSmokeTP->SetVisibility(false); }
}
