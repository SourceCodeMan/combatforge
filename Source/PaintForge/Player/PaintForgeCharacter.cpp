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
}

void APaintForgeCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PFMovement != nullptr)
	{
		PFMovement->OnSlideStateChanged.RemoveAll(this);
	}
	GetWorldTimerManager().ClearTimer(SprintOutTimerHandle);
	GetWorldTimerManager().ClearTimer(BufferedJumpClearHandle);

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

void APaintForgeCharacter::SetTeamColor(uint8 TeamId)
{
	CachedTeamId = TeamId;
	const FLinearColor TeamColor = PFColors::ForTeam(TeamId);

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
