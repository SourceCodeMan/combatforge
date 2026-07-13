// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "AI/PFBotController.h"

#include "PaintForge.h"
#include "Player/PaintForgeCharacter.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"
#include "Core/PaintForgeTypes.h"
#include "Combat/PFWeaponComponent.h"
#include "Objectives/PFControlPointActor.h"
#include "Objectives/PFFlagActor.h"
#include "Objectives/PFObjectiveLayout.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

// Live playtest knob: override skill for newly spawned bots (takes effect next build/round). -1 = default.
static TAutoConsoleVariable<int32> CVarBotSkill(
	TEXT("pf.BotSkill"),
	-1,
	TEXT("Bot difficulty for newly spawned bots: -1=default, 0=Rookie(easy), 1=Regular, 2=Sharpshooter."),
	ECVF_Default);

APFBotController::APFBotController()
{
	bWantsPlayerState = true;                       // → APaintForgePlayerState in PlayerArray (team/alive/spread-seed)
	bSetControlRotationFromPawnOrientation = false; // we aim by SetControlRotation each tick; don't fight it
	PrimaryActorTick.bCanEverTick = true;
}

APaintForgeCharacter* APFBotController::GetBotCharacter() const
{
	return Cast<APaintForgeCharacter>(GetPawn());
}

void APFBotController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	CurrentTarget = nullptr;
	bFiring = false;
	FireHoldTimer = 0.f;
	ApplySkill();
}

void APFBotController::OnUnPossess()
{
	SetFiring(false);
	Super::OnUnPossess();
}

void APFBotController::ApplySkill()
{
	// One knob for the whole bot difficulty. Rookie is deliberately soft for the kids' session: LAGGY aim
	// (low turn rate → misses strafing players), wide error, a long "notice" delay, and a short engage
	// range. Regular is the middle brain; Sharpshooter tightens it for scrims. pf.BotSkill overrides live.
	EPFBotSkill Effective = Skill;
	const int32 Override = CVarBotSkill.GetValueOnAnyThread();
	if (Override >= 0 && Override <= static_cast<int32>(EPFBotSkill::Sharpshooter))
	{
		Effective = static_cast<EPFBotSkill>(Override);
	}
	switch (Effective)
	{
	case EPFBotSkill::Rookie:
		AimErrorDeg = 14.f;  AimTurnRate = 2.5f;  ReactionDelay = 0.60f;  EngageRangeUU = 3200.f;  break;
	case EPFBotSkill::Sharpshooter:
		AimErrorDeg = 1.5f;  AimTurnRate = 11.f;  ReactionDelay = 0.12f;  EngageRangeUU = 5500.f;  break;
	case EPFBotSkill::Regular:
	default:
		AimErrorDeg = 4.5f;  AimTurnRate = 6.5f;  ReactionDelay = 0.30f;  EngageRangeUU = 4500.f;  break;
	}
}

void APFBotController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	APaintForgeCharacter* Bot = GetBotCharacter();
	APaintForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<APaintForgeGameState>() : nullptr;
	if (Bot == nullptr || GS == nullptr)
	{
		return;
	}
	const APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>();

	// Only fight during a live combat round while alive; otherwise idle (freeze/build/vote/results/out).
	const bool bAlive = (PS != nullptr) && PS->bAliveInRound;
	const bool bCombatLive = (GS->Phase == EPFMatchPhase::Combat)
		&& (GS->RoundState == EPFRoundState::Live) && GS->IsFireAllowed();
	if (!bAlive || !bCombatLive)
	{
		SetFiring(false);
		return;
	}

	// Bots can't walk to the [E] ammo barrels, so once their 150-ball supply ran dry they'd roam the rest of
	// the match without firing (BeginReload no-ops at ReserveAmmo==0). Give AI effectively infinite ammo: top
	// the weapon back to full whenever total ammo dips to a mag or less. Authority-only (bot Tick = host) and
	// ServerRefillFromPickup no-ops when already full, so this is cheap.
	if (UPFWeaponComponent* Weapon = Bot->GetWeapon())
	{
		if (Weapon->GetTotalAmmo() <= static_cast<int32>(Weapon->HopperCapacity))
		{
			Weapon->ServerRefillFromPickup();
		}
	}

	// Sticky targeting: keep the current enemy while it's still alive, in range and visible; only re-scan
	// (throttled) when we have no engageable target. This stops the nearest-enemy rank from thrashing
	// between two near-equidistant foes — which otherwise re-armed the reaction gap every 0.4s refresh and
	// froze the trigger (fatal for Rookie, whose 0.6s notice gap exceeds the refresh). The aim error re-roll
	// and the reaction "notice" gap are applied only on a genuine target change.
	TargetRefreshTimer -= DeltaSeconds;
	FireHoldTimer -= DeltaSeconds;
	if (!IsTargetEngageable(CurrentTarget.Get()) && TargetRefreshTimer <= 0.f)
	{
		TargetRefreshTimer = TargetRefreshInterval;
		APaintForgeCharacter* PrevTarget = CurrentTarget.Get();
		APaintForgeCharacter* NewTarget = AcquireNearestEnemy();   // nearest VISIBLE enemy (else nearest)
		if (NewTarget != PrevTarget && NewTarget != nullptr)
		{
			FireHoldTimer = ReactionDelay;   // notice gap only when we actually switch to a new target
		}
		CurrentTarget = NewTarget;
	}

	APaintForgeCharacter* Target = CurrentTarget.Get();

	// Objective goal (Domination / Hardpoint / CTF): where this bot should push, even with no enemy in
	// sight. Fight modes leave it unset, so the bot only acts when it has an enemy.
	FVector ObjGoal;
	const bool bHasObjective = ComputeObjectiveGoal(ObjGoal);
	if (Target == nullptr && !bHasObjective)
	{
		SetFiring(false);
		return;   // nothing to fight and no objective to push — idle
	}

	// --- Aim + fire (only with an enemy). Re-roll a random error every AimJitterInterval, then EASE control
	// rotation toward the target at a capped turn rate so bots lag strafers and miss. Fire in range, past the
	// reaction gap, with LOS. The weapon + server dir-gate read control rotation, so this IS the shot dir.
	if (Target != nullptr)
	{
		const FVector BotEye = Bot->GetActorLocation() + FVector(0.f, 0.f, 60.f);
		const FVector TargetChest = Target->GetActorLocation() + FVector(0.f, 0.f, 40.f);
		const FVector ToTarget = TargetChest - BotEye;
		const float Dist = ToTarget.Size();

		AimJitterTimer -= DeltaSeconds;
		if (AimJitterTimer <= 0.f)
		{
			AimJitterYaw = FMath::FRandRange(-AimErrorDeg, AimErrorDeg);
			AimJitterPitch = FMath::FRandRange(-AimErrorDeg, AimErrorDeg) * 0.5f;
			AimJitterTimer = AimJitterInterval;
		}
		FRotator DesiredAim = ToTarget.Rotation();
		DesiredAim.Yaw += AimJitterYaw;
		DesiredAim.Pitch = FMath::Clamp(DesiredAim.Pitch + AimJitterPitch, -80.f, 80.f);
		SetControlRotation(FMath::RInterpTo(GetControlRotation(), DesiredAim, DeltaSeconds, AimTurnRate));

		SetFiring((Dist <= EngageRangeUU) && (FireHoldTimer <= 0.f) && HasLineOfSight(Target));
	}
	else
	{
		SetFiring(false);   // pushing an objective with nobody in sight — hold fire
	}

	// --- Movement: pick a heading, steer it around cover, break out if pinned. In objective modes the bot
	// heads for its point/flag (still fighting on the way); otherwise it holds a stand-off band on the enemy.
	StrafeTimer -= DeltaSeconds;
	if (StrafeTimer <= 0.f)
	{
		StrafeSign = (FMath::FRand() < 0.5f) ? -1.f : 1.f;
		StrafeTimer = StrafeSwitchInterval;
	}
	const FVector BotLoc = Bot->GetActorLocation();
	FVector Flat;          // primary heading (also drives the unstick escape)
	FVector DesiredDir;
	if (bHasObjective)
	{
		Flat = FVector(ObjGoal.X - BotLoc.X, ObjGoal.Y - BotLoc.Y, 0.f).GetSafeNormal();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Flat);
		if (FVector::Dist2D(BotLoc, ObjGoal) > ObjectiveHoldRadiusUU) { DesiredDir = (Flat + Right * StrafeSign * 0.25f).GetSafeNormal(); }
		else                                                          { DesiredDir = Right * StrafeSign; }   // on the point: hold + stay dodgy
	}
	else
	{
		const FVector ToEnemy = Target->GetActorLocation() - BotLoc;
		Flat = FVector(ToEnemy.X, ToEnemy.Y, 0.f).GetSafeNormal();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Flat);
		const float Dist = ToEnemy.Size2D();
		if (Dist > PreferredRangeUU)   { DesiredDir = (Flat + Right * StrafeSign * 0.4f).GetSafeNormal(); }
		else if (Dist < MinRangeUU)    { DesiredDir = -Flat; }
		else                           { DesiredDir = Right * StrafeSign; }
	}

	// Unstick: if we wanted to move but barely have over the last sample window, peel off sideways a beat.
	StuckSampleTimer -= DeltaSeconds;
	if (EscapeTimer > 0.f)
	{
		EscapeTimer -= DeltaSeconds;
		DesiredDir = (FVector::CrossProduct(FVector::UpVector, Flat) * EscapeSign - Flat * 0.35f).GetSafeNormal();
	}
	else if (StuckSampleTimer <= 0.f)
	{
		if (!StuckSamplePos.IsZero()
			&& FVector::DistSquared2D(BotLoc, StuckSamplePos) < FMath::Square(StuckMoveThresh))
		{
			EscapeTimer = 0.7f;
			EscapeSign = (FMath::FRand() < 0.5f) ? -1.f : 1.f;
		}
		StuckSamplePos = BotLoc;
		StuckSampleTimer = 0.5f;
	}

	Bot->AddMovementInput(SteerAvoidingObstacles(DesiredDir), 1.f);
}

APaintForgeCharacter* APFBotController::AcquireNearestEnemy() const
{
	const APaintForgeCharacter* Bot = GetBotCharacter();
	const APaintForgePlayerState* MyPS = GetPlayerState<APaintForgePlayerState>();
	const APaintForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<APaintForgeGameState>() : nullptr;
	if (Bot == nullptr || MyPS == nullptr || GS == nullptr)
	{
		return nullptr;
	}
	const uint8 MyTeam = MyPS->TeamId;
	// Free-for-All has no teams — every other living combatant is a target. Team modes keep the
	// teammate/unassigned filter. (Reads MatchType only; the FFA rules themselves live in GameMode.)
	const bool bFFA = (GS->MatchType == EPFMatchType::FreeForAll);

	// Track the nearest VISIBLE enemy and, separately, the nearest of any — so a bot prefers a foe it can
	// actually shoot, but still advances toward the closest when none are currently in sight.
	APaintForgeCharacter* BestVisible = nullptr;   float BestVisibleSq = TNumericLimits<float>::Max();
	APaintForgeCharacter* BestAny = nullptr;       float BestAnySq = TNumericLimits<float>::Max();
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* OtherPS = Cast<APaintForgePlayerState>(PSBase);
		if (OtherPS == nullptr || OtherPS == MyPS)
		{
			continue;
		}
		if (!OtherPS->bAliveInRound)
		{
			continue;   // already out
		}
		if (!bFFA && (OtherPS->TeamId > 1 || OtherPS->TeamId == MyTeam))
		{
			continue;   // team modes: skip teammate / unassigned
		}
		APaintForgeCharacter* OtherChar = Cast<APaintForgeCharacter>(OtherPS->GetPawn());
		if (OtherChar == nullptr)
		{
			continue;
		}
		const float DistSq = FVector::DistSquared(Bot->GetActorLocation(), OtherChar->GetActorLocation());
		if (DistSq < BestAnySq)
		{
			BestAnySq = DistSq;
			BestAny = OtherChar;
		}
		if (DistSq < BestVisibleSq && HasLineOfSight(OtherChar))
		{
			BestVisibleSq = DistSq;
			BestVisible = OtherChar;
		}
	}
	return BestVisible != nullptr ? BestVisible : BestAny;
}

bool APFBotController::IsTargetEngageable(const APaintForgeCharacter* Target) const
{
	if (Target == nullptr)
	{
		return false;
	}
	const APaintForgePlayerState* TargetPS = Target->GetPlayerState<APaintForgePlayerState>();
	if (TargetPS == nullptr || !TargetPS->bAliveInRound)
	{
		return false;   // dead / despawned → drop it and re-scan
	}
	const APaintForgeCharacter* Bot = GetBotCharacter();
	if (Bot == nullptr)
	{
		return false;
	}
	if (FVector::DistSquared(Bot->GetActorLocation(), Target->GetActorLocation()) > FMath::Square(EngageRangeUU))
	{
		return false;   // drifted out of engage range → let it re-scan for a nearer foe
	}
	return HasLineOfSight(Target);   // hold the target only while we can still see it
}

void APFBotController::EnsureObjectivesCached()
{
	// Objectives are spawned once per match; only (re)scan while our cache is empty/stale — this covers a
	// bot that existed before the objective actors spawned, and a fresh match. Counts are tiny (<=3 CP, <=2 flag).
	const bool bHavePoints = ControlPointsCache.Num() > 0 && ControlPointsCache[0].IsValid();
	const bool bHaveFlags  = FlagsCache.Num() > 0 && FlagsCache[0].IsValid();
	if (bHavePoints || bHaveFlags)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	ControlPointsCache.Reset();
	FlagsCache.Reset();
	for (TActorIterator<APFControlPointActor> It(World); It; ++It) { ControlPointsCache.Add(*It); }
	for (TActorIterator<APFFlagActor> It(World); It; ++It)          { FlagsCache.Add(*It); }
}

bool APFBotController::ComputeObjectiveGoal(FVector& OutGoal)
{
	const APaintForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<APaintForgeGameState>() : nullptr;
	const APaintForgePlayerState* MyPS = GetPlayerState<APaintForgePlayerState>();
	const APaintForgeCharacter* Bot = GetBotCharacter();
	if (GS == nullptr || MyPS == nullptr || Bot == nullptr)
	{
		return false;
	}
	const EPFMatchType Mode = GS->MatchType;
	if (Mode != EPFMatchType::Domination && Mode != EPFMatchType::Hardpoint && Mode != EPFMatchType::CaptureFlag)
	{
		return false;   // fight modes have no objective goal
	}
	EnsureObjectivesCached();
	const uint8 MyTeam = MyPS->TeamId;
	const FVector BotLoc = Bot->GetActorLocation();

	if (Mode == EPFMatchType::CaptureFlag)
	{
		// Find my home flag (for the home location) and check whether I'm carrying the enemy flag.
		APFFlagActor* HomeFlag = nullptr;
		bool bCarryingEnemyFlag = false;
		for (const TWeakObjectPtr<APFFlagActor>& FlagPtr : FlagsCache)
		{
			APFFlagActor* Flag = FlagPtr.Get();
			if (Flag == nullptr) { continue; }
			if (Flag->GetOwnerTeam() == MyTeam) { HomeFlag = Flag; }
			else if (Flag->GetCarrier() == MyPS) { bCarryingEnemyFlag = true; }
		}
		const FVector MyHome = HomeFlag ? HomeFlag->GetHomeLocation() : PFObjectiveLayout::FlagHome(MyTeam);
		if (bCarryingEnemyFlag)
		{
			OutGoal = MyHome;   // run it home to score
			return true;
		}
		// Not carrying: go grab the nearest grabbable (not-carried) enemy flag.
		APFFlagActor* BestFlag = nullptr;
		float BestSq = TNumericLimits<float>::Max();
		for (const TWeakObjectPtr<APFFlagActor>& FlagPtr : FlagsCache)
		{
			APFFlagActor* Flag = FlagPtr.Get();
			if (Flag == nullptr || Flag->GetOwnerTeam() == MyTeam || Flag->IsCarried()) { continue; }
			const float DSq = FVector::DistSquared(BotLoc, Flag->GetActorLocation());
			if (DSq < BestSq) { BestSq = DSq; BestFlag = Flag; }
		}
		OutGoal = BestFlag ? BestFlag->GetActorLocation() : MyHome;   // else escort/defend near home
		return true;
	}

	// Domination / Hardpoint: head for a control point.
	APFControlPointActor* GoalCP = nullptr;
	if (Mode == EPFMatchType::Hardpoint)
	{
		float BestSq = TNumericLimits<float>::Max();
		for (const TWeakObjectPtr<APFControlPointActor>& CPPtr : ControlPointsCache)
		{
			APFControlPointActor* CP = CPPtr.Get();
			if (CP == nullptr || !CP->IsPointActive()) { continue; }
			const float DSq = FVector::DistSquared(BotLoc, CP->GetActorLocation());
			if (DSq < BestSq) { BestSq = DSq; GoalCP = CP; }
		}
	}
	else   // Domination: nearest point we don't already own (else nearest to defend)
	{
		float BestUnownedSq = TNumericLimits<float>::Max();
		float BestAnySq = TNumericLimits<float>::Max();
		APFControlPointActor* NearestAny = nullptr;
		for (const TWeakObjectPtr<APFControlPointActor>& CPPtr : ControlPointsCache)
		{
			APFControlPointActor* CP = CPPtr.Get();
			if (CP == nullptr) { continue; }
			const float DSq = FVector::DistSquared(BotLoc, CP->GetActorLocation());
			if (DSq < BestAnySq) { BestAnySq = DSq; NearestAny = CP; }
			if (CP->GetControllingTeam() != MyTeam && DSq < BestUnownedSq) { BestUnownedSq = DSq; GoalCP = CP; }
		}
		if (GoalCP == nullptr) { GoalCP = NearestAny; }
	}
	if (GoalCP != nullptr)
	{
		OutGoal = GoalCP->GetActorLocation();
		return true;
	}
	return false;
}

bool APFBotController::HasLineOfSight(const APaintForgeCharacter* Target) const
{
	const APaintForgeCharacter* Bot = GetBotCharacter();
	UWorld* World = GetWorld();
	if (Bot == nullptr || Target == nullptr || World == nullptr)
	{
		return false;
	}
	const FVector Start = Bot->GetActorLocation() + FVector(0.f, 0.f, 60.f);
	const FVector End = Target->GetActorLocation() + FVector(0.f, 0.f, 40.f);
	FCollisionQueryParams Params(FName(TEXT("BotLOS")), /*bTraceComplex=*/false, Bot);
	Params.AddIgnoredActor(Target);
	FHitResult Hit;
	// Trace ignores self + target: a blocking hit means cover/geometry is in the way → no line of sight.
	const bool bBlocked = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
	return !bBlocked;
}

FVector APFBotController::SteerAvoidingObstacles(const FVector& DesiredDir) const
{
	const APaintForgeCharacter* Bot = GetBotCharacter();
	UWorld* World = GetWorld();
	if (Bot == nullptr || World == nullptr || DesiredDir.IsNearlyZero())
	{
		return DesiredDir;
	}
	const FVector Origin = Bot->GetActorLocation() + FVector(0.f, 0.f, 30.f);
	FCollisionQueryParams Params(FName(TEXT("BotAvoid")), /*bTraceComplex=*/false, Bot);
	const FCollisionShape Probe = FCollisionShape::MakeSphere(AvoidProbeRadius);
	// A heading is "clear" if a short forward sphere-sweep (≈ the pawn's width) hits no static geometry
	// (cover, walls, perimeter). A sphere, not a thin line, so convex corners and low props are caught
	// before the capsule bumps them.
	auto PathClear = [&](const FVector& Dir) -> bool
	{
		FHitResult Hit;
		return !World->SweepSingleByChannel(Hit, Origin, Origin + Dir * AvoidProbeUU,
			FQuat::Identity, ECC_WorldStatic, Probe, Params);
	};
	if (PathClear(DesiredDir))
	{
		return DesiredDir;
	}
	// Blocked ahead: fan out to the nearest clear heading, alternating sides across the FULL arc (including
	// behind), so a pinned bot actually finds the open direction instead of guessing a rear heading.
	static const float Sweep[] = { 35.f, -35.f, 60.f, -60.f, 90.f, -90.f, 130.f, -130.f, 160.f, -160.f, 180.f };
	for (const float Angle : Sweep)
	{
		const FVector Candidate = DesiredDir.RotateAngleAxis(Angle, FVector::UpVector);
		if (PathClear(Candidate))
		{
			return Candidate;
		}
	}
	return FVector::ZeroVector;   // fully boxed in — stop pushing into geometry; the unstick timer re-rolls
}

void APFBotController::SetFiring(bool bFire)
{
	if (bFire == bFiring)
	{
		return;
	}
	bFiring = bFire;
	if (APaintForgeCharacter* Bot = GetBotCharacter())
	{
		// ADS while shooting: tighter spread + CMC ADS flag (reads as aiming, not hip-fire).
		// TP rifle pose is driven by GetBaseAimRotation every tick either way.
		Bot->SetADS(bFire);

		if (UPFWeaponComponent* Weapon = Bot->GetWeapon())
		{
			if (bFire)
			{
				Weapon->StartFire();
			}
			else
			{
				Weapon->StopFire();
			}
		}
	}
}
