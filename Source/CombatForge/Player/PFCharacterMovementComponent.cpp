// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PFCharacterMovementComponent.h"

#include "CombatForge.h"
#include "Components/CapsuleComponent.h"   // mantle: ledge detection uses the live capsule dims
#include "Engine/World.h"
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

	// Capsule depenetration ceilings (engine defaults 100/500). Even with GetPenetrationAdjustment
	// zeroing pawn-vs-pawn Z, a large MaxDepenetration budget still lets geometry MTD fling a
	// jammed bot skyward when path-follow drives them into a wall corner. Keep nudges tiny.
	MaxDepenetrationWithGeometry = 40.f;
	MaxDepenetrationWithGeometryAsProxy = 40.f;
	MaxDepenetrationWithPawn = 10.f;
	MaxDepenetrationWithPawnAsProxy = 10.f;

	NavAgentProps.bCanCrouch = true;
	SetCrouchedHalfHeight(58.f);
	bCanWalkOffLedgesWhenCrouching = true;
	// Crouch camera smoothing (88->58 over 0.2 s) is composed on the character's
	// camera — UE capsule resize itself is instant and must stay instant for
	// prediction determinism.

	bWantsToSprintPF = false;
	bWantsToADSPF = false;
	bWantsToMantlePF = false;
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

void UPFCharacterMovementComponent::SetWantsToMantle(bool bWants)
{
	bWantsToMantlePF = bWants;
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

bool UPFCharacterMovementComponent::IsMantling() const
{
	return MovementMode == MOVE_Custom && CustomMovementMode == CMOVE_Mantle;
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
	if (IsMantling())
	{
		return 0.f;   // the climb interp drives motion, not acceleration
	}
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
		// Per-weapon weight (kit-replicated; identical on server + autonomous proxy).
		Speed *= FMath::Clamp(CachedWeaponMoveSpeedMult, 0.5f, 1.1f);
		return Speed;
	}
	default:
		return Super::GetMaxSpeed();
	}
}

float UPFCharacterMovementComponent::GetMaxBrakingDeceleration() const
{
	if (IsSliding() || IsMantling())
	{
		return 0.f; // PhysSlide applies its own friction curve; PhysMantle is a pure interp
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
	// No jump-cancel out of a climb.
	if (IsMantling())
	{
		return false;
	}
	// Allow jumping out of the slide (slide-jump keeps horizontal velocity:
	// stock DoJump only sets Velocity.Z, then falls — exactly what 04 §1.2 asks).
	if (IsSliding())
	{
		return IsJumpAllowed();
	}
	return Super::CanAttemptJump();
}

void UPFCharacterMovementComponent::OnTeleported()
{
	Super::OnTeleported();
	// A respawn/eject teleport mid-climb must not keep interpolating toward the STALE target
	// (the pawn would glide back across the map). Fall; FindFloor sorts out the rest.
	if (IsMantling())
	{
		SetMovementMode(MOVE_Falling);
	}
}

FVector UPFCharacterMovementComponent::GetPenetrationAdjustment(const FHitResult& Hit) const
{
	FVector Adjust = Super::GetPenetrationAdjustment(Hit);

	// "Bots randomly launch to the roof" (Tom, recurring). Overlapping capsules resolve via a NON-SWEPT teleport
	// along the minimum-translation direction every sub-step. Bots get driven into each other / wall corners;
	// once horizontal escape is blocked the MTD flips VERTICAL and the pawn rockets skyward (Velocity never
	// written → looks like an instant launch, not a jump). Players don't hit this because they aren't AI-commanded
	// into other capsules.
	//
	// Hard rules:
	//  1) Pawn-vs-pawn: NEVER resolve on Z — horizontal separation only.
	//  2) Geometry: clamp Z tightly so a corner jam cannot become a roof-launch.
	//  3) Absolute magnitude clamp so a single sub-step can't teleport hundreds of uu.
	const bool bVsPawn = (Cast<APawn>(Hit.GetActor()) != nullptr)
		|| (Hit.Component.IsValid() && Hit.Component->GetCollisionObjectType() == ECC_Pawn);
	if (bVsPawn)
	{
		Adjust.Z = 0.f;
		// Prefer pure horizontal push (drop any residual vertical from Super).
		Adjust = FVector(Adjust.X, Adjust.Y, 0.f);
	}
	else
	{
		Adjust.Z = FMath::Clamp(Adjust.Z, -12.f, 12.f);
	}
	// Absolute per-step cap (on top of MaxDepenetration* properties).
	const float MaxStep = bVsPawn ? 12.f : 24.f;
	if (Adjust.SizeSquared() > FMath::Square(MaxStep))
	{
		Adjust = Adjust.GetSafeNormal() * MaxStep;
	}
	return Adjust;
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
	// crouch wanted + sprint flag + FORWARD intent + grounded + speed >= 750 + cooldown elapsed.
	// The forward-hemisphere test mirrors IsSprintingEffective (issue #13 B8: the raw sprint flag let a
	// sideways strafe enter a slide sprint never actually powered) — inlined rather than calling it,
	// because IsSprintingEffective() also tests IsCrouching(), which is in flux on the entry frame.
	// Reads only Acceleration + the component transform, both replayed, so prediction stays deterministic.
	const FVector SlideAccelDir = Acceleration.GetSafeNormal2D();
	const bool bSlideForwardIntent = UpdatedComponent != nullptr
		&& !SlideAccelDir.IsNearlyZero()
		&& FVector::DotProduct(SlideAccelDir, UpdatedComponent->GetForwardVector().GetSafeNormal2D()) > 0.5f;
	if (!IsSliding()
		&& bWantsToCrouch
		&& bWantsToSprintPF
		&& bSlideForwardIntent
		&& IsMovingOnGround()
		&& Velocity.Size2D() >= SlideMinEnterSpeed
		&& SlideCooldownRemaining <= 0.f)
	{
		EnterSlide();
	}

	// Deterministic mantle entry (04-adjacent, Tom 2026-07-17): the 2nd SPACE press while airborne rides
	// FLAG_Custom_2; while it's held and we're falling, retry the ledge test each sim step — geometry is
	// STATIC world collision, so client prediction and server replay agree. The flag is a pure input intent
	// (cleared by the character on key release, never mutated inside the sim).
	if (!IsMantling()
		&& bWantsToMantlePF
		&& IsFalling())
	{
		FVector StandTarget;
		if (DetectMantleLedge(StandTarget))
		{
			EnterMantle(StandTarget);
		}
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

void UPFCharacterMovementComponent::EnterMantle(const FVector& StandTarget)
{
	MantleStart   = UpdatedComponent->GetComponentLocation();
	MantleTarget  = StandTarget;
	MantleElapsed = 0.f;
	Velocity      = FVector::ZeroVector;   // the interp owns the motion for MantleTime
	// The climb target/clearance were computed for the STANDING capsule (DetectMantleLedge); dropping the
	// crouch intent here keeps the capsule state deterministic through the mode change (a mid-climb forced
	// un-crouch would re-expand the capsule into the ledge).
	bWantsToCrouch = false;
	SetMovementMode(MOVE_Custom, CMOVE_Mantle);
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

	const bool bWasMantling = (PreviousMovementMode == MOVE_Custom && PreviousCustomMode == CMOVE_Mantle);
	const bool bNowMantling = IsMantling();
	if (bNowMantling && !bWasMantling)
	{
		OnMantleStateChanged.Broadcast(true);
	}
	else if (bWasMantling && !bNowMantling)
	{
		MantleElapsed = 0.f;
		OnMantleStateChanged.Broadcast(false);
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
	else if (CustomMovementMode == CMOVE_Mantle)
	{
		PhysMantle(deltaTime, Iterations);
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
// Mantle physics + ledge detection
// ---------------------------------------------------------------------------

void UPFCharacterMovementComponent::PhysMantle(float deltaTime, int32 Iterations)
{
	if (deltaTime < PFMinTickTime || !HasValidData())
	{
		return;
	}

	// Stale-target guard (review wf_e923820a): a network mode-apply can put a machine into CMOVE_Mantle
	// WITHOUT it having run EnterMantle (e.g. a client receiving a server correction whose mode is Mantle),
	// leaving MantleTarget zero/ancient — interpolating there would glide the pawn across the map. Bail to
	// falling; the authoritative position stream carries the actual climb.
	if (MantleTarget.IsNearlyZero()
		|| FVector::DistSquared(MantleTarget, UpdatedComponent->GetComponentLocation()) > FMath::Square(600.f))
	{
		SetMovementMode(MOVE_Falling);
		return;
	}

	Iterations++;
	bJustTeleported = true;   // interp-driven: never derive velocity from the positional delta

	MantleElapsed += deltaTime;
	const float Duration = FMath::Max(0.05f, MantleTime);
	const float Alpha = FMath::Clamp(MantleElapsed / Duration, 0.f, 1.f);

	// Rise-then-tuck: vertical dominates the first 60% (clear the lip), then translate forward onto/over the
	// ledge. Both eased so the climb reads as a grab-and-pull, not a linear glide. Fully deterministic from
	// (MantleStart, MantleTarget, MantleElapsed) — a mid-climb correction replays the same curve.
	const float ZAlpha  = FMath::InterpEaseOut(0.f, 1.f, FMath::Clamp(Alpha / 0.6f, 0.f, 1.f), 2.f);
	const float XYAlpha = FMath::InterpEaseInOut(0.f, 1.f, FMath::Clamp((Alpha - 0.6f) / 0.4f, 0.f, 1.f), 2.f);
	FVector NewLoc;
	NewLoc.X = FMath::Lerp(MantleStart.X, MantleTarget.X, XYAlpha);
	NewLoc.Y = FMath::Lerp(MantleStart.Y, MantleTarget.Y, XYAlpha);
	NewLoc.Z = FMath::Lerp(MantleStart.Z, MantleTarget.Z, ZAlpha);

	// Collision OFF for the climb (bSweep=false): the end position was pre-validated clear, and sweeping
	// would snag the capsule on the very lip we're climbing.
	MoveUpdatedComponent(NewLoc - UpdatedComponent->GetComponentLocation(),
		UpdatedComponent->GetComponentQuat(), /*bSweep=*/false);
	Velocity = FVector::ZeroVector;

	if (Alpha >= 1.f)
	{
		// Hand off with a small forward carry: over a THIN wall the capsule (fully above the top) drops down
		// the far side — the "climb over"; onto a thick slab/floor top, FindFloor lands it next step.
		const FVector Fwd = UpdatedComponent->GetForwardVector().GetSafeNormal2D();
		Velocity = Fwd * 150.f;
		SetMovementMode(MOVE_Falling);
	}
}

bool UPFCharacterMovementComponent::DetectMantleLedge(FVector& OutStandTarget) const
{
	if (!HasValidData() || CharacterOwner == nullptr)
	{
		return false;
	}
	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	UWorld* World = GetWorld();
	if (Capsule == nullptr || World == nullptr)
	{
		return false;
	}
	const float Radius = Capsule->GetScaledCapsuleRadius();
	// Feet come from the LIVE capsule (crouched or not), but the target + clearance use the STANDING dims:
	// EnterMantle drops the crouch, so the capsule that arrives at the ledge is the 88 one — sizing the
	// clearance for a crouched 58 capsule would embed the re-expanded capsule in the ledge (review wf_e923820a).
	const float CurHalfHeight   = Capsule->GetScaledCapsuleHalfHeight();
	const float StandHalfHeight = FMath::Max(CurHalfHeight, CharacterOwner->GetDefaultHalfHeight());
	const FVector Loc  = UpdatedComponent->GetComponentLocation();
	const FVector Fwd  = UpdatedComponent->GetForwardVector().GetSafeNormal2D();
	if (Fwd.IsNearlyZero())
	{
		return false;
	}
	const float FeetZ = Loc.Z - CurHalfHeight;

	// STATIC world geometry only (build pieces + shell are ECC_WorldStatic objects): pawns can't be mantled,
	// and — critically — static-only keeps the test deterministic between client prediction + server replay.
	FCollisionObjectQueryParams StaticOnly(ECC_WorldStatic);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PFMantle), /*bTraceComplex=*/false, CharacterOwner);

	// 1) A near-vertical face directly ahead at chest height.
	const FVector ChestStart(Loc.X, Loc.Y, FeetZ + 55.f);
	FHitResult WallHit;
	if (!World->LineTraceSingleByObjectType(WallHit, ChestStart,
		ChestStart + Fwd * (Radius + MantleReachUU), StaticOnly, Params))
	{
		return false;
	}
	if (WallHit.ImpactNormal.Z > 0.35f || FVector::DotProduct(FVector(WallHit.ImpactNormal), Fwd) > -0.3f)
	{
		return false;   // a floor/ramp surface or a glancing wall — not a mantle face
	}

	// 2) Ledge top height: sample just past the hit face (and a backup on the face plane). Prefer the
	//    highest solid within the one-level band so a thin wall top wins over a ramp deeper inside the cell.
	const float ProbeTopZ = FeetZ + MantleMaxHeightUU + 80.f;
	auto SampleTopZ = [&](const FVector& XY, float& OutZ) -> bool
	{
		FHitResult Hit;
		if (!World->LineTraceSingleByObjectType(Hit,
			FVector(XY.X, XY.Y, ProbeTopZ),
			FVector(XY.X, XY.Y, FeetZ - 20.f), StaticOnly, Params))
		{
			return false;
		}
		OutZ = Hit.ImpactPoint.Z;
		return true;
	};

	float LedgeTopZ = 0.f;
	bool bHaveTop = false;
	// Samples near the face (wall top). Avoid going deep into the far cell first — that often hits a ramp
	// slope and rejects a valid 1-wall climb.
	const FVector FaceN = FVector(WallHit.ImpactNormal).GetSafeNormal2D();
	const FVector SamplePts[] = {
		FVector(WallHit.ImpactPoint) - FaceN * 6.f,           // slightly into the wall volume / top
		FVector(WallHit.ImpactPoint) + Fwd * 6.f,             // just past the face
		FVector(WallHit.ImpactPoint) + Fwd * 14.f,
	};
	for (const FVector& P : SamplePts)
	{
		float Z = 0.f;
		if (!SampleTopZ(P, Z))
		{
			continue;
		}
		const float Rel = Z - FeetZ;
		if (Rel < MantleMinHeightUU || Rel > MantleMaxHeightUU)
		{
			continue;
		}
		if (!bHaveTop || Z > LedgeTopZ)
		{
			LedgeTopZ = Z;
			bHaveTop = true;
		}
	}
	if (!bHaveTop)
	{
		return false;   // no one-level lip (step-up, too tall, or no top)
	}

	// 3) Landing: try several spots past the face. Prefer standing ON whatever surface is there (floor,
	//    far-side ramp, wall top) rather than requiring empty air — a ramp behind a 1-high wall used to
	//    fail the full-capsule overlap even though the climb is legal.
	const FCollisionShape ClearShape = FCollisionShape::MakeCapsule(
		FMath::Max(10.f, Radius - 6.f), FMath::Max(12.f, StandHalfHeight - 6.f));
	const FCollisionShape PathShape = FCollisionShape::MakeCapsule(
		FMath::Max(10.f, Radius - 10.f), FMath::Max(10.f, StandHalfHeight - 10.f));

	const float FwdOffsets[] = {
		10.f,
		18.f,
		Radius * 0.35f + 12.f,
		Radius + 12.f,
		Radius + 28.f,
		Radius + 48.f,
	};

	FVector BestStand = FVector::ZeroVector;
	bool bFound = false;
	for (const float Off : FwdOffsets)
	{
		const FVector LandXY = FVector(WallHit.ImpactPoint) + Fwd * Off;
		// Surface under this XY: ramp / floor / wall top.
		float SurfZ = LedgeTopZ;
		{
			FHitResult FloorHit;
			const bool bFloor = World->LineTraceSingleByObjectType(FloorHit,
				FVector(LandXY.X, LandXY.Y, LedgeTopZ + 220.f),
				FVector(LandXY.X, LandXY.Y, LedgeTopZ - 320.f), StaticOnly, Params);
			if (bFloor)
			{
				SurfZ = FloorHit.ImpactPoint.Z;
			}
		}
		// Must stay near the grabbed lip: not a distant lower floor, not a roof far above.
		if (SurfZ > LedgeTopZ + 100.f || SurfZ < LedgeTopZ - 280.f)
		{
			continue;
		}

		const FVector StandCandidate(
			LandXY.X, LandXY.Y, SurfZ + StandHalfHeight + 4.f);
		// Vertical rise corner: same XY as start, stand height (clear the lip).
		const FVector RiseCorner(Loc.X, Loc.Y, FMath::Max(StandCandidate.Z, LedgeTopZ + StandHalfHeight + 4.f));

		if (World->OverlapBlockingTestByChannel(RiseCorner, FQuat::Identity, ECC_Pawn, ClearShape, Params))
		{
			continue;   // no headroom to rise (true roof)
		}
		if (World->OverlapBlockingTestByChannel(StandCandidate, FQuat::Identity, ECC_Pawn, ClearShape, Params))
		{
			continue;   // landing capsule intersects solid
		}
		// Path: up, then over. Shrunk so flush wall scrapes don't false-block.
		if (World->SweepTestByChannel(Loc, RiseCorner, FQuat::Identity, ECC_Pawn, PathShape, Params)
			|| World->SweepTestByChannel(RiseCorner, StandCandidate, FQuat::Identity, ECC_Pawn, PathShape, Params))
		{
			continue;
		}

		BestStand = StandCandidate;
		bFound = true;
		break;
	}

	// 4) Last resort for thin walls: vault target ABOVE the lip with only a short forward tuck (then fall
	//    onto the far-side ramp/floor). Overlap against far geometry is ignored as long as the rise is clear
	//    and a small air pocket above the lip is free.
	if (!bFound)
	{
		const FVector VaultXY = FVector(WallHit.ImpactPoint) + Fwd * 16.f;
		const FVector VaultStand(VaultXY.X, VaultXY.Y, LedgeTopZ + StandHalfHeight + 12.f);
		const FVector RiseCorner(Loc.X, Loc.Y, VaultStand.Z);
		const FCollisionShape AirShape = FCollisionShape::MakeCapsule(
			FMath::Max(8.f, Radius - 12.f), FMath::Max(10.f, StandHalfHeight * 0.55f));
		if (!World->OverlapBlockingTestByChannel(RiseCorner, FQuat::Identity, ECC_Pawn, ClearShape, Params)
			&& !World->OverlapBlockingTestByChannel(VaultStand, FQuat::Identity, ECC_Pawn, AirShape, Params)
			&& !World->SweepTestByChannel(Loc, RiseCorner, FQuat::Identity, ECC_Pawn, PathShape, Params))
		{
			// Horizontal tuck may graze a far ramp — allow; PhysMantle ends in Falling with forward carry.
			BestStand = VaultStand;
			bFound = true;
		}
	}

	if (!bFound)
	{
		return false;
	}

	OutStandTarget = BestStand;
	return true;
}

// ---------------------------------------------------------------------------
// Compressed flags / saved moves (02 D5, R6)
// ---------------------------------------------------------------------------

void UPFCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);

	bWantsToSprintPF = (Flags & FSavedMove_Character::FLAG_Custom_0) != 0;
	bWantsToADSPF    = (Flags & FSavedMove_Character::FLAG_Custom_1) != 0;
	bWantsToMantlePF = (Flags & FSavedMove_Character::FLAG_Custom_2) != 0;
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
	, bSavedWantsToMantle(false)
{
}

void FSavedMove_PF::Clear()
{
	Super::Clear();
	bSavedWantsToSprint = false;
	bSavedWantsToADS = false;
	bSavedWantsToMantle = false;
	SavedMantleStart = FVector::ZeroVector;
	SavedMantleTarget = FVector::ZeroVector;
	SavedMantleElapsed = 0.f;
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
	if (bSavedWantsToMantle)
	{
		Result |= FLAG_Custom_2;
	}
	return Result;
}

bool FSavedMove_PF::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const
{
	const FSavedMove_PF* NewMovePF = static_cast<const FSavedMove_PF*>(NewMove.Get());
	if (bSavedWantsToSprint != NewMovePF->bSavedWantsToSprint
		|| bSavedWantsToADS != NewMovePF->bSavedWantsToADS
		|| bSavedWantsToMantle != NewMovePF->bSavedWantsToMantle)
	{
		return false;
	}
	// Never combine moves whose mantle sim state differs — each mid-climb move must replay with the
	// elapsed/target it originally had (both are all-zero outside a mantle, so normal moves still combine).
	if (SavedMantleElapsed != NewMovePF->SavedMantleElapsed
		|| SavedMantleTarget != NewMovePF->SavedMantleTarget
		|| SavedMantleStart != NewMovePF->SavedMantleStart)
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
		bSavedWantsToMantle = CMC->bWantsToMantlePF;
		SavedMantleStart = CMC->MantleStart;
		SavedMantleTarget = CMC->MantleTarget;
		SavedMantleElapsed = CMC->MantleElapsed;
	}
}

void FSavedMove_PF::PrepMoveFor(ACharacter* C)
{
	Super::PrepMoveFor(C);

	if (UPFCharacterMovementComponent* CMC = Cast<UPFCharacterMovementComponent>(C->GetCharacterMovement()))
	{
		CMC->bWantsToSprintPF = bSavedWantsToSprint;
		CMC->bWantsToADSPF = bSavedWantsToADS;
		CMC->bWantsToMantlePF = bSavedWantsToMantle;
		CMC->MantleStart = SavedMantleStart;
		CMC->MantleTarget = SavedMantleTarget;
		CMC->MantleElapsed = SavedMantleElapsed;
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
