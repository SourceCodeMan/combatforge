// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PFCharacterMovementComponent.h"

#include "CombatForge.h"
#include "GameFramework/Character.h"

namespace
{
	constexpr float PFMinTickTime = 1e-6f; // mirrors CMC's internal MIN_TICK_TIME
}

UPFCharacterMovementComponent::UPFCharacterMovementComponent()
{
	// 04 §1.1 locomotion numbers — set once here, tuned only through the
	// UPROPERTYs above. GetMaxSpeed() below is the only live speed source.
	MaxWalkSpeed = 600.f;
	MaxWalkSpeedCrouched = 300.f;
	MaxAcceleration = 4096.f;
	GroundFriction = 10.f;
	BrakingDecelerationWalking = 2500.f;
	BrakingFrictionFactor = 1.0f;
	JumpZVelocity = 630.f;
	GravityScale = 1.5f;
	AirControl = 0.9f;
	PerchRadiusThreshold = 15.f;

	NavAgentProps.bCanCrouch = true;
	SetCrouchedHalfHeight(58.f);
	bCanWalkOffLedgesWhenCrouching = true;
	// Crouch camera smoothing (88->58 over 0.2 s) is composed on the character's
	// camera — UE capsule resize itself is instant and must stay instant for
	// prediction determinism.

	bWantsToSprintPF = false;
	bWantsToADSPF = false;
	bSlideGlideActive = false;
}

// ---------------------------------------------------------------------------
// Input intents
// ---------------------------------------------------------------------------

void UPFCharacterMovementComponent::SetWantsToSprint(bool bWants)
{
	bWantsToSprintPF = bWants;
}

void UPFCharacterMovementComponent::SetWantsToADS(bool bWants)
{
	bWantsToADSPF = bWants;
}

void UPFCharacterMovementComponent::OnCrouchSlidePressed()
{
	bWantsToCrouch = true;
}

void UPFCharacterMovementComponent::OnCrouchSlideReleased()
{
	bWantsToCrouch = false;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool UPFCharacterMovementComponent::IsSliding() const
{
	return MovementMode == MOVE_Custom && CustomMovementMode == CMOVE_Slide;
}

bool UPFCharacterMovementComponent::IsSprintingEffective() const
{
	if (!bWantsToSprintPF || !IsMovingOnGround() || IsCrouching() || UpdatedComponent == nullptr)
	{
		return false;
	}
	const FVector AccelDir = Acceleration.GetSafeNormal2D();
	if (AccelDir.IsNearlyZero())
	{
		return false;
	}
	const FVector Forward = UpdatedComponent->GetForwardVector().GetSafeNormal2D();
	return FVector::DotProduct(AccelDir, Forward) > 0.5f; // forward hemisphere only (04 §1.1)
}

bool UPFCharacterMovementComponent::IsSlideGlidePhase() const
{
	return IsSliding() && bSlideGlideActive;
}

bool UPFCharacterMovementComponent::WantsToADS() const
{
	return bWantsToADSPF;
}

// ---------------------------------------------------------------------------
// Speed — the single source of truth (02 R6)
// ---------------------------------------------------------------------------

float UPFCharacterMovementComponent::GetMaxSpeed() const
{
	if (IsSliding())
	{
		return SlideBoostSpeed;
	}

	switch (MovementMode)
	{
	case MOVE_Walking:
	case MOVE_NavWalking:
	case MOVE_Falling:
	{
		float Speed;
		if (IsCrouching())
		{
			Speed = CrouchSpeed;
		}
		else if (IsSprintingEffective())
		{
			Speed = SprintSpeed;
		}
		else
		{
			Speed = WalkSpeed;
		}
		// ADS multiplier stacks multiplicatively (ADS+crouch = 165 — 04 §1.1);
		// sprint and ADS are mutually exclusive at the character layer, but
		// guard anyway so the flag combination can never yield fast-ADS.
		if (bWantsToADSPF && !IsSprintingEffective())
		{
			Speed *= ADSSpeedMult;
		}
		return Speed;
	}
	default:
		return Super::GetMaxSpeed();
	}
}

float UPFCharacterMovementComponent::GetMaxBrakingDeceleration() const
{
	if (IsSliding())
	{
		return 0.f; // PhysSlide applies its own friction curve
	}
	return Super::GetMaxBrakingDeceleration();
}

// ---------------------------------------------------------------------------
// Crouch / jump interaction with the slide
// ---------------------------------------------------------------------------

bool UPFCharacterMovementComponent::CanCrouchInCurrentState() const
{
	// Stock implementation refuses in MOVE_Custom, which would auto-uncrouch us
	// mid-slide; the slide uses the crouched capsule for its full duration.
	if (IsSliding())
	{
		return CanEverCrouch();
	}
	return Super::CanCrouchInCurrentState();
}

bool UPFCharacterMovementComponent::CanAttemptJump() const
{
	// Allow jumping out of the slide (slide-jump keeps horizontal velocity:
	// stock DoJump only sets Velocity.Z, then falls — exactly what 04 §1.2 asks).
	if (IsSliding())
	{
		return IsJumpAllowed();
	}
	return Super::CanAttemptJump();
}

// ---------------------------------------------------------------------------
// Slide entry / exit
// ---------------------------------------------------------------------------

void UPFCharacterMovementComponent::OnMovementUpdated(float DeltaSeconds, const FVector& OldLocation, const FVector& OldVelocity)
{
	Super::OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);

	if (!HasValidData())
	{
		return;
	}

	// Cooldown counts down only while grounded (0.5 s of ground time — 04 §1.2).
	if (SlideCooldownRemaining > 0.f && IsMovingOnGround())
	{
		SlideCooldownRemaining = FMath::Max(0.f, SlideCooldownRemaining - DeltaSeconds);
	}

	// Deterministic slide entry — same inputs on client and server:
	// crouch wanted + sprint flag + grounded + speed >= 750 + cooldown elapsed.
	if (!IsSliding()
		&& bWantsToCrouch
		&& bWantsToSprintPF
		&& IsMovingOnGround()
		&& Velocity.Size2D() >= SlideMinEnterSpeed
		&& SlideCooldownRemaining <= 0.f)
	{
		EnterSlide();
	}
}

void UPFCharacterMovementComponent::EnterSlide()
{
	FVector Dir = Velocity.GetSafeNormal2D();
	if (Dir.IsNearlyZero())
	{
		Dir = UpdatedComponent ? UpdatedComponent->GetForwardVector().GetSafeNormal2D() : FVector::ForwardVector;
	}

	// Entry boost along current velocity direction, NOT aim (04 §1.2).
	Velocity = Dir * SlideBoostSpeed;

	SlideElapsed = 0.f;
	SlideRampStartElapsed = SlideGlideTime;
	bSlideGlideActive = true;

	SetMovementMode(MOVE_Custom, CMOVE_Slide);
}

void UPFCharacterMovementComponent::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);

	const bool bWasSliding = (PreviousMovementMode == MOVE_Custom && PreviousCustomMode == CMOVE_Slide);
	const bool bNowSliding = IsSliding();

	if (bNowSliding && !bWasSliding)
	{
		OnSlideStateChanged.Broadcast(true);
	}
	else if (bWasSliding && !bNowSliding)
	{
		SlideCooldownRemaining = SlideCooldown;
		bSlideGlideActive = false;
		OnSlideStateChanged.Broadcast(false);
	}
}

// ---------------------------------------------------------------------------
// Slide physics
// ---------------------------------------------------------------------------

void UPFCharacterMovementComponent::PhysCustom(float deltaTime, int32 Iterations)
{
	Super::PhysCustom(deltaTime, Iterations);

	if (CustomMovementMode == CMOVE_Slide)
	{
		PhysSlide(deltaTime, Iterations);
	}
	else
	{
		UE_LOG(CombatForgeLog, Error, TEXT("PhysCustom: unknown custom movement mode %d"), CustomMovementMode);
		SetMovementMode(MOVE_Walking);
	}
}

void UPFCharacterMovementComponent::PhysSlide(float deltaTime, int32 Iterations)
{
	if (deltaTime < PFMinTickTime || !HasValidData())
	{
		return;
	}

	SlideElapsed += deltaTime;

	// -- Ground check: no walkable floor -> fall out of the slide.
	FFindFloorResult Floor;
	FindFloor(UpdatedComponent->GetComponentLocation(), Floor, false);
	if (!Floor.IsWalkableFloor())
	{
		SetMovementMode(MOVE_Falling);
		StartNewPhysics(deltaTime, Iterations);
		return;
	}
	CurrentFloor = Floor;

	// -- End conditions: crouch released, speed floor, hard cap (04 §1.2).
	if (!bWantsToCrouch || SlideElapsed >= SlideMaxDuration || Velocity.Size2D() < SlideMinExitSpeed)
	{
		SetMovementMode(MOVE_Walking);
		StartNewPhysics(deltaTime, Iterations);
		return;
	}

	Iterations++;
	bJustTeleported = false;

	// -- Steering: input bends the heading at most SlideSteerDegPerSec; the
	//    camera aims freely and never drives the slide direction.
	const FVector AccelDir = Acceleration.GetSafeNormal2D();
	FVector VelDir2D = Velocity.GetSafeNormal2D();
	if (!AccelDir.IsNearlyZero() && !VelDir2D.IsNearlyZero())
	{
		const float MaxSteerDeg = SlideSteerDegPerSec * deltaTime;
		const float Dot = FMath::Clamp(FVector::DotProduct(VelDir2D, AccelDir), -1.f, 1.f);
		const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(Dot));
		const float SteerDeg = FMath::Min(AngleDeg, MaxSteerDeg);
		const float Sign = (FVector::CrossProduct(VelDir2D, AccelDir).Z >= 0.f) ? 1.f : -1.f;
		const FVector NewDir = VelDir2D.RotateAngleAxis(SteerDeg * Sign, FVector::UpVector);
		Velocity = NewDir * Velocity.Size2D() + FVector(0.f, 0.f, Velocity.Z);
	}

	// -- Gravity along the slope (accelerates downhill slides).
	const FVector FloorNormal = CurrentFloor.HitResult.ImpactNormal;
	Velocity += FVector::VectorPlaneProject(FVector(0.f, 0.f, GetGravityZ()), FloorNormal) * deltaTime;

	// -- Friction curve: glide 0.6 for the first 0.35 s, extended while
	//    descending (velocity pointing > 0.15 below horizontal), then a lerp
	//    to 8.0 over SlideFrictionRampTime (04 §1.2).
	const bool bDownhill = Velocity.GetSafeNormal().Z < -0.15f;
	float Friction;
	if (SlideElapsed <= SlideGlideTime || bDownhill)
	{
		Friction = SlideGlideFriction;
		SlideRampStartElapsed = SlideElapsed;
		bSlideGlideActive = true;
	}
	else
	{
		const float RampAlpha = FMath::Clamp((SlideElapsed - SlideRampStartElapsed) / SlideFrictionRampTime, 0.f, 1.f);
		Friction = FMath::Lerp(SlideGlideFriction, SlideEndFriction, RampAlpha);
		bSlideGlideActive = false;
	}
	Velocity *= FMath::Max(0.f, 1.f - Friction * deltaTime);

	// -- Clamp: the entry boost is the slide's speed ceiling.
	if (Velocity.Size() > SlideBoostSpeed)
	{
		Velocity = Velocity.GetSafeNormal() * SlideBoostSpeed;
	}

	// -- Move.
	const FVector PreMoveLocation = UpdatedComponent->GetComponentLocation();
	const FVector Delta = Velocity * deltaTime;
	FHitResult Hit(1.f);
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		HandleImpact(Hit, deltaTime, Delta);
		SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
	}

	// Keep Velocity coherent with the actual move so the next frame's end
	// conditions see post-collision speed.
	if (!bJustTeleported)
	{
		Velocity = (UpdatedComponent->GetComponentLocation() - PreMoveLocation) / deltaTime;
	}
}

// ---------------------------------------------------------------------------
// Compressed flags / saved moves (02 D5, R6)
// ---------------------------------------------------------------------------

void UPFCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);

	bWantsToSprintPF = (Flags & FSavedMove_Character::FLAG_Custom_0) != 0;
	bWantsToADSPF    = (Flags & FSavedMove_Character::FLAG_Custom_1) != 0;
}

FNetworkPredictionData_Client* UPFCharacterMovementComponent::GetPredictionData_Client() const
{
	// During client teardown / disconnect, the engine can still query prediction data with a null
	// PawnOwner. A hard check() here hard-crashes kids' clients mid-match with no dialog.
	if (PawnOwner == nullptr)
	{
		UE_LOG(CombatForgeLog, Warning,
			TEXT("PFCMC::GetPredictionData_Client: PawnOwner null (teardown?) — returning cached data"));
		return ClientPredictionData;
	}

	if (ClientPredictionData == nullptr)
	{
		UPFCharacterMovementComponent* MutableThis = const_cast<UPFCharacterMovementComponent*>(this);
		MutableThis->ClientPredictionData = new FNetworkPredictionData_Client_PF(*this);
	}
	return ClientPredictionData;
}

// ---- FSavedMove_PF ----

FSavedMove_PF::FSavedMove_PF()
	: bSavedWantsToSprint(false)
	, bSavedWantsToADS(false)
{
}

void FSavedMove_PF::Clear()
{
	Super::Clear();
	bSavedWantsToSprint = false;
	bSavedWantsToADS = false;
}

uint8 FSavedMove_PF::GetCompressedFlags() const
{
	uint8 Result = Super::GetCompressedFlags();
	if (bSavedWantsToSprint)
	{
		Result |= FLAG_Custom_0;
	}
	if (bSavedWantsToADS)
	{
		Result |= FLAG_Custom_1;
	}
	return Result;
}

bool FSavedMove_PF::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const
{
	const FSavedMove_PF* NewMovePF = static_cast<const FSavedMove_PF*>(NewMove.Get());
	if (bSavedWantsToSprint != NewMovePF->bSavedWantsToSprint
		|| bSavedWantsToADS != NewMovePF->bSavedWantsToADS)
	{
		return false;
	}
	return Super::CanCombineWith(NewMove, InCharacter, MaxDelta);
}

void FSavedMove_PF::SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, FNetworkPredictionData_Client_Character& ClientData)
{
	Super::SetMoveFor(C, InDeltaTime, NewAccel, ClientData);

	if (const UPFCharacterMovementComponent* CMC = Cast<UPFCharacterMovementComponent>(C->GetCharacterMovement()))
	{
		bSavedWantsToSprint = CMC->bWantsToSprintPF;
		bSavedWantsToADS = CMC->bWantsToADSPF;
	}
}

void FSavedMove_PF::PrepMoveFor(ACharacter* C)
{
	Super::PrepMoveFor(C);

	if (UPFCharacterMovementComponent* CMC = Cast<UPFCharacterMovementComponent>(C->GetCharacterMovement()))
	{
		CMC->bWantsToSprintPF = bSavedWantsToSprint;
		CMC->bWantsToADSPF = bSavedWantsToADS;
	}
}

// ---- FNetworkPredictionData_Client_PF ----

FNetworkPredictionData_Client_PF::FNetworkPredictionData_Client_PF(const UCharacterMovementComponent& ClientMovement)
	: Super(ClientMovement)
{
}

FSavedMovePtr FNetworkPredictionData_Client_PF::AllocateNewMove()
{
	return FSavedMovePtr(new FSavedMove_PF());
}
