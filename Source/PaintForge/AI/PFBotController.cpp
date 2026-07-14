// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "AI/PFBotController.h"

#include "PaintForge.h"
#include "AI/PFSquadSubsystem.h"
#include "Player/PaintForgeCharacter.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"
#include "Core/PaintForgeTypes.h"
#include "Combat/PFWeaponComponent.h"
#include "Combat/PFHealthComponent.h"
#include "Objectives/PFControlPointActor.h"
#include "Objectives/PFFlagActor.h"
#include "Objectives/PFObjectiveLayout.h"

#include "Engine/World.h"
#include "Engine/Engine.h"   // GEngine->AddOnScreenDebugMessage (pf.NavCheck on-screen readout)
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "Navigation/PathFollowingComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Perception/AISense_Hearing.h"

// Live playtest knob: override skill for newly spawned bots (takes effect next build/round). -1 = default.
static TAutoConsoleVariable<int32> CVarBotSkill(
	TEXT("pf.BotSkill"),
	-1,
	TEXT("Bot difficulty for newly spawned bots: -1=default, 0=Rookie(easy), 1=Regular, 2=Sharpshooter."),
	ECVF_Default);

// Nav diagnostic: run `pf.NavCheck` in the console to confirm the runtime navmesh generated over the arena
// and covers the local player. The authoritative test is ProjectPointToNavigation (is there mesh under me?).
static FAutoConsoleCommandWithWorld GNavCheckCmd(
	TEXT("pf.NavCheck"),
	TEXT("Report whether the runtime navmesh is present and covers the local player (AI pathfinding diagnostic)."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}
		UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		APlayerController* PC = World->GetFirstPlayerController();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		const FVector Loc = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
		FNavLocation Projected;
		const bool bOnMesh = (Nav != nullptr) && (Pawn != nullptr)
			&& Nav->ProjectPointToNavigation(Loc, Projected, FVector(300.f, 300.f, 600.f));
		UE_LOG(PaintForgeLog, Warning,
			TEXT("pf.NavCheck: NavSystem=%s Pawn=%s PlayerOnNavmesh=%s @ %s%s"),
			Nav ? TEXT("yes") : TEXT("NULL"),
			Pawn ? TEXT("yes") : TEXT("NULL"),
			bOnMesh ? TEXT("YES") : TEXT("no"),
			*Loc.ToCompactString(),
			bOnMesh ? *FString::Printf(TEXT(" (mesh @ %s)"), *Projected.Location.ToCompactString()) : TEXT(""));
		// Also print on-screen so it's visible in-game without opening the log.
		if (GEngine != nullptr)
		{
			const FColor Col = bOnMesh ? FColor::Green : FColor::Red;
			GEngine->AddOnScreenDebugMessage(-1, 8.f, Col,
				FString::Printf(TEXT("pf.NavCheck: navmesh=%s  PlayerOnNavmesh=%s"),
					Nav ? TEXT("present") : TEXT("MISSING"),
					bOnMesh ? TEXT("YES (pathfinding live)") : TEXT("NO (bots can't path here)")));
		}
	}));

APFBotController::APFBotController()
{
	bWantsPlayerState = true;                       // → APaintForgePlayerState in PlayerArray (team/alive/spread-seed)
	bSetControlRotationFromPawnOrientation = false; // we aim by SetControlRotation each tick; don't fight it
	PrimaryActorTick.bCanEverTick = true;

	// ---- AI Perception: sight cone + hearing. Friend/foe comes from GetTeamAttitudeTowards (game teams). ----
	AIPerception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerception"));
	SetPerceptionComponent(*AIPerception);

	SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius = SightRadiusUU;
	SightConfig->LoseSightRadius = SightLoseRadiusUU;                 // >= SightRadius (hysteresis)
	SightConfig->PeripheralVisionAngleDegrees = SightFOVHalfDeg;      // HALF-angle → 2x total FOV
	SightConfig->AutoSuccessRangeFromLastSeenLocation = SightAutoSeeUU;
	SightConfig->SetMaxAge(SearchHoldSec);                           // remember a lost target this long
	SightConfig->DetectionByAffiliation.bDetectEnemies = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals = false;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = false;
	AIPerception->ConfigureSense(*SightConfig);
	AIPerception->SetDominantSense(SightConfig->GetSenseImplementation());

	HearingConfig = CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("HearingConfig"));
	HearingConfig->HearingRange = HearingRangeUU;
	HearingConfig->DetectionByAffiliation.bDetectEnemies = true;
	HearingConfig->DetectionByAffiliation.bDetectNeutrals = true;    // a gunshot is a gunshot — hear it, then investigate
	HearingConfig->DetectionByAffiliation.bDetectFriendlies = false;
	AIPerception->ConfigureSense(*HearingConfig);
}

void APFBotController::BeginPlay()
{
	Super::BeginPlay();
	if (AIPerception != nullptr)
	{
		AIPerception->OnTargetPerceptionUpdated.AddDynamic(this, &APFBotController::OnPerceptionStimulus);
	}
	HearingSenseID = UAISense::GetSenseID<UAISense_Hearing>();
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
	bFireModeAssigned = false;   // re-pick the fire mode for the new pawn (its ApplyWeaponLoadout reset the default)
	LastPathedGoal = FVector::ZeroVector;   // force a fresh path on the new pawn's first tactical goal
	RepathTimer = 0.f;
	LastSeenTime = InvestigateTime = -1000.f;   // clear stale search memory from a previous life
	bHaveTacticalGoal = false;   ReposTimer = 0.f;   // re-evaluate firing position for the new pawn
	LastKnownHP = 255;   RoundMaxHP = 1;   SuppressedUntil = -1000.f;   // fresh health/suppression for the new life
	// Give this bot a team id so perception has a concrete affiliation (attitude itself comes from the
	// GetTeamAttitudeTowards override, but a real id avoids any NoTeam short-circuit in the sense filter).
	if (const APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>())
	{
		SetGenericTeamId(FGenericTeamId(PS->TeamId <= 1 ? PS->TeamId : 2));
	}
	ApplySkill();
}

void APFBotController::OnUnPossess()
{
	SetFiring(false);
	StopMovement();   // drop any in-flight path so the freed pawn doesn't keep walking
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
	// An eliminated pawn awaiting its respawn timer must go LIMP: Skirmish never clears bAliveInRound, so
	// without the bEliminated check the bot keeps steering its own corpse — it wanders off the KillZ ledge,
	// the pawn is destroyed, and the weak-pointer respawn is stranded (bot freezes forever).
	const UPFHealthComponent* Health = Bot->GetHealth();
	const bool bAlive = (PS != nullptr) && PS->bAliveInRound
		&& (Health == nullptr || !Health->bEliminated);
	const bool bCombatLive = (GS->Phase == EPFMatchPhase::Combat)
		&& (GS->RoundState == EPFRoundState::Live) && GS->IsFireAllowed();
	if (!bAlive || !bCombatLive)
	{
		SetFiring(false);
		StopMovement();   // halt path-following — a MoveTo left running would keep walking the (dead/frozen) pawn
		return;
	}

	const float NowSec = (GetWorld() != nullptr) ? GetWorld()->GetTimeSeconds() : 0.f;

	// Suppression: detect taking a hit (HP dropped since last tick) → mark "under fire" (widens aim below) and,
	// on a fresh hit, break for a new tactical position (cover). RoundMaxHP is the injury denominator this life.
	if (Health != nullptr)
	{
		RoundMaxHP = FMath::Max(RoundMaxHP, Health->HP);
		if (LastKnownHP != 255 && Health->HP < LastKnownHP)
		{
			const bool bWasSuppressed = (NowSec < SuppressedUntil);
			SuppressedUntil = NowSec + SuppressDurationSec;
			if (!bWasSuppressed) { bHaveTacticalGoal = false; }   // just got shot → reposition to cover now
		}
		LastKnownHP = Health->HP;
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
		// One-time per spawn: pick a fire mode from the equipped weapon's ALLOWED set, keyed by roster index so
		// some bots run auto, some burst, some single (deterministic, evenly spread). Runs after the pawn's
		// BeginPlay->ApplyWeaponLoadout has set the allowed mask.
		if (!bFireModeAssigned && PS != nullptr)
		{
			bFireModeAssigned = true;
			TArray<EPFFireMode, TInlineAllocator<3>> Allowed;
			for (uint8 m = 0; m < 3; ++m)
			{
				if (Weapon->IsFireModeAllowed(static_cast<EPFFireMode>(m)))
				{
					Allowed.Add(static_cast<EPFFireMode>(m));
				}
			}
			if (Allowed.Num() > 0)
			{
				Weapon->SetFireMode(Allowed[PS->RosterIndex % Allowed.Num()]);
			}
		}
	}

	// Target selection: prefer the nearest VISIBLE enemy, but KEEP the current one unless a clearly-closer foe
	// appears (20% hysteresis) so we don't thrash between two near-equidistant enemies — that used to re-arm the
	// reaction gap every refresh and freeze the trigger. CRITICAL FIX: the old code only re-scanned when the
	// current target went disengageable, so a bot locked on a FAR enemy ignored someone who ran up point-blank
	// ("run right up and they don't shoot you"). Now we always re-scan, and an enemy inside PointBlankUU
	// immediately becomes the target (a bot must turn on someone in its face) with a near-instant reaction gap.
	TargetRefreshTimer -= DeltaSeconds;
	FireHoldTimer -= DeltaSeconds;
	NavWarnTimer -= DeltaSeconds;
	if (TargetRefreshTimer <= 0.f)
	{
		TargetRefreshTimer = TargetRefreshInterval;
		APaintForgeCharacter* Curr = CurrentTarget.Get();
		const bool bCurrOK = IsTargetEngageable(Curr);
		APaintForgeCharacter* Best = AcquireNearestEnemy();   // nearest VISIBLE enemy (else nearest)
		if (Best != nullptr && Best != Curr)
		{
			const FVector BotAt = Bot->GetActorLocation();
			const float BestD = FVector::Dist(BotAt, Best->GetActorLocation());
			const float CurrD = bCurrOK ? FVector::Dist(BotAt, Curr->GetActorLocation()) : TNumericLimits<float>::Max();
			// Switch if we have no valid target, OR the new one is clearly closer, OR it's point-blank.
			if (!bCurrOK || BestD < CurrD * 0.8f || BestD < PointBlankUU)
			{
				FireHoldTimer = (BestD < PointBlankUU) ? (ReactionDelay * 0.35f) : ReactionDelay;
				CurrentTarget = Best;
				bHaveTacticalGoal = false;   // new target → re-pick a firing position now, don't reuse the old one
			}
		}
		else if (Best == nullptr && !bCurrOK)
		{
			CurrentTarget = nullptr;
		}
	}

	APaintForgeCharacter* Target = CurrentTarget.Get();

	// Last-known-position: remember where we last SAW the target, so when LOS breaks the bot hunts that spot
	// instead of instantly forgetting. Updated only while the target is actually visible.
	if (Target != nullptr && HasLineOfSight(Target))
	{
		LastSeenPos = Target->GetActorLocation();
		LastSeenTime = NowSec;
		// Squad awareness: tell the team where this enemy is so teammates without a target converge on it.
		if (PS != nullptr && PS->TeamId <= 1)
		{
			if (UPFSquadSubsystem* Squad = GetWorld()->GetSubsystem<UPFSquadSubsystem>())
			{
				Squad->ReportEnemy(PS->TeamId, Target, LastSeenPos, NowSec);
			}
		}
	}

	// Objective goal (Domination / Hardpoint / CTF): where this bot should push, even with no enemy in sight.
	FVector ObjGoal;
	const bool bHasObjective = ComputeObjectiveGoal(ObjGoal);

	// Search goal: the more RECENT of "where I last saw you" and "where I heard a noise", while still fresh — so a
	// bot that lost sight (you ducked behind cover) or was shot from behind goes to hunt/investigate, not idle.
	const bool bSeenFresh = (NowSec - LastSeenTime) < SearchHoldSec;
	const bool bNoiseFresh = (NowSec - InvestigateTime) < SearchHoldSec;
	bool bHaveSearch = bSeenFresh || bNoiseFresh;
	FVector SearchPos = bHaveSearch
		? ((bSeenFresh && (!bNoiseFresh || LastSeenTime >= InvestigateTime)) ? LastSeenPos : InvestigatePos)
		: FVector::ZeroVector;

	// Squad coordination: with no target and no personal lead, borrow the team's nearest fresh sighting — a
	// teammate saw someone, so go help instead of wandering to centre. Team modes only (FFA bots are solo).
	if (!bHaveSearch && Target == nullptr && PS != nullptr && PS->TeamId <= 1)
	{
		if (const UPFSquadSubsystem* Squad = GetWorld()->GetSubsystem<UPFSquadSubsystem>())
		{
			FVector Lead;
			if (Squad->GetSharedLead(PS->TeamId, Bot->GetActorLocation(), NowSec, SearchHoldSec, nullptr, Lead))
			{
				SearchPos = Lead;
				bHaveSearch = true;
			}
		}
	}

	const FVector BotLoc = Bot->GetActorLocation();

	// Nothing to fight, push, or investigate → SEEK CONTACT: push toward the arena centre (where fights happen)
	// while panning the view, rather than standing at spawn until an enemy wanders into the sight cone. Since
	// perception only reports enemies the bot can actually see/sense, without this a Skirmish/FFA bot with no one
	// in view would freeze. Hold + slow-scan once at the centre.
	if (Target == nullptr && !bHasObjective && !bHaveSearch)
	{
		SetFiring(false);
		const FVector ArenaCenter(3200.f, 2000.f, BotLoc.Z);
		if (FVector::Dist2D(BotLoc, ArenaCenter) > 800.f)
		{
			const FVector ToC = ArenaCenter - (BotLoc + FVector(0.f, 0.f, 60.f));
			if (!ToC.IsNearlyZero())
			{
				SetControlRotation(FMath::RInterpTo(GetControlRotation(), ToC.Rotation(), DeltaSeconds, 4.f));
			}
			RepathTimer -= DeltaSeconds;
			if (RepathTimer <= 0.f || GetMoveStatus() == EPathFollowingStatus::Idle)
			{
				MoveToGoal(ArenaCenter, nullptr);
				RepathTimer = RepathInterval;
			}
		}
		else
		{
			StopMovement();
			FRotator ScanAim = GetControlRotation();
			ScanAim.Yaw += DeltaSeconds * 40.f;   // slow pan to sweep for enemies
			SetControlRotation(ScanAim);
		}
		return;
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

		// Aim quality scales with range: at distance the bot stays deliberately laggy + wide (beatable), but as
		// the target closes it tracks faster and tightens up — so a point-blank enemy circling the bot actually
		// gets tracked and hit instead of walking around a slow, 14°-wide Rookie aim.
		const float CloseT = (Dist < CloseAimRangeUU) ? (1.f - Dist / CloseAimRangeUU) : 0.f;   // 0 at edge → 1 at contact
		const float EffTurnRate = AimTurnRate * FMath::Lerp(1.f, CloseAimTurnMult, CloseT);
		// Accuracy under pressure: injured (low HP) + suppressed (recently shot) bots aim WIDER — so trading fire
		// wears a bot down and staying on target rewards you (Gray-Zone-style situational accuracy).
		const float HealthFrac = (Health != nullptr && RoundMaxHP > 0) ? static_cast<float>(Health->HP) / static_cast<float>(RoundMaxHP) : 1.f;
		float AccPenalty = FMath::Lerp(1.f, InjuryErrorMaxMult, 1.f - HealthFrac);
		if (NowSec < SuppressedUntil) { AccPenalty *= SuppressErrorMult; }
		const float EffAimError = AimErrorDeg * FMath::Lerp(1.f, CloseAimErrorMult, CloseT) * AccPenalty;

		AimJitterTimer -= DeltaSeconds;
		if (AimJitterTimer <= 0.f)
		{
			AimJitterYaw = FMath::FRandRange(-EffAimError, EffAimError);
			AimJitterPitch = FMath::FRandRange(-EffAimError, EffAimError) * 0.5f;
			AimJitterTimer = AimJitterInterval * FMath::Lerp(1.f, 0.4f, CloseT);   // re-roll faster up close so the tighter cone applies quickly
		}
		FRotator DesiredAim = ToTarget.Rotation();
		DesiredAim.Yaw += AimJitterYaw;
		DesiredAim.Pitch = FMath::Clamp(DesiredAim.Pitch + AimJitterPitch, -80.f, 80.f);
		SetControlRotation(FMath::RInterpTo(GetControlRotation(), DesiredAim, DeltaSeconds, EffTurnRate));

		const bool bLOS = HasLineOfSight(Target);
		const bool bWantFire = (Dist <= EngageRangeUU) && (FireHoldTimer <= 0.f) && bLOS;
		SetFiring(bWantFire);

		// Diagnostic: if an enemy is point-blank but we're NOT firing, log why (LOS / reaction gap / range) so a
		// remaining "won't shoot in your face" case is pinpointed instead of guessed. Throttled (shares NavWarnTimer).
		if (!bWantFire && Dist < PointBlankUU && NavWarnTimer <= 0.f)
		{
			NavWarnTimer = 3.f;
			UE_LOG(PaintForgeLog, Warning,
				TEXT("Bot NOT firing point-blank: dist=%.0f LOS=%d fireHold=%.2f inRange=%d"),
				Dist, bLOS ? 1 : 0, FireHoldTimer, (Dist <= EngageRangeUU) ? 1 : 0);
		}
	}
	else
	{
		// No target: hold fire and turn toward where we're heading (search LKP / objective) so the sight cone
		// covers that direction — a bot hunting your last-known-position stares at where it thinks you are.
		SetFiring(false);
		const FVector LookAt = bHaveSearch ? SearchPos : ObjGoal;
		const FVector ToLook = LookAt - (BotLoc + FVector(0.f, 0.f, 60.f));
		if (!ToLook.IsNearlyZero())
		{
			SetControlRotation(FMath::RInterpTo(GetControlRotation(), ToLook.Rotation(), DeltaSeconds, 4.f));
		}
	}

	// --- Movement: choose a tactical GOAL POINT by mode, then let the navmesh path there (MoveToGoal routes the
	// bot AROUND the fort). Facing is owned by the aim code above; MoveToGoal strafes so it never overrides yaw.
	StrafeTimer -= DeltaSeconds;
	if (StrafeTimer <= 0.f)
	{
		StrafeSign = (FMath::FRand() < 0.5f) ? -1.f : 1.f;
		StrafeTimer = StrafeSwitchInterval;
	}

	const float SearchReachUU = 250.f;   // within this of a hunt/investigate point counts as "arrived"
	FVector GoalLoc = BotLoc;
	bool bMove = false;
	if (Target != nullptr)
	{
		// FIGHT: reposition to the best nearby firing position (LOS + cover + range + flank + spread), re-evaluated
		// periodically. This replaces the old strafe-in-the-open with deliberate tactical movement — bots work to
		// cover, hold angles, and flank as a group. Aim/fire (above) keeps the enemy tracked while we move.
		ReposTimer -= DeltaSeconds;
		if (!bHaveTacticalGoal || ReposTimer <= 0.f)
		{
			TacticalGoal = ChooseTacticalPosition(Target);
			bHaveTacticalGoal = true;
			ReposTimer = RepositionInterval;
		}
		GoalLoc = TacticalGoal;
		bMove = true;
	}
	else if (bHasObjective && FVector::Dist2D(BotLoc, ObjGoal) > ObjectiveHoldRadiusUU)
	{
		GoalLoc = ObjGoal;   // push the objective
		bMove = true;
	}
	else if (bHaveSearch && FVector::Dist2D(BotLoc, SearchPos) > SearchReachUU)
	{
		GoalLoc = SearchPos;   // hunt the last-known-position / investigate a heard noise
		bMove = true;
	}
	else if (bHasObjective)
	{
		// On the objective with nobody in sight: hold + strafe (stay dodgy).
		const FVector Flat = FVector(ObjGoal.X - BotLoc.X, ObjGoal.Y - BotLoc.Y, 0.f).GetSafeNormal();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Flat);
		GoalLoc = BotLoc + Right * StrafeSign * GoalProjectUU;
		bMove = true;
	}
	// else: reached the search point / nothing to move to — stand and scan (StopMovement below; aim handled above).

	if (bMove)
	{
		// Throttle re-pathing: re-issue only when the goal drifted enough OR we've gone idle. Re-issuing every
		// tick aborts the prior request and rebuilds the path → visible jitter.
		RepathTimer -= DeltaSeconds;
		const bool bGoalMoved = FVector::DistSquared(GoalLoc, LastPathedGoal) > FMath::Square(RepathMoveThreshUU);
		const bool bIdle = (GetMoveStatus() == EPathFollowingStatus::Idle);
		if ((RepathTimer <= 0.f && bGoalMoved) || bIdle)
		{
			MoveToGoal(GoalLoc, Target);   // Target may be null (search/objective) → MoveToGoal just paths to GoalLoc
			RepathTimer = RepathInterval;
		}
	}
	else
	{
		StopMovement();
	}
}

void APFBotController::MoveToGoal(const FVector& RawGoal, AActor* FallbackActor)
{
	LastPathedGoal = RawGoal;

	// The tactical goal is a heading projected ~700uu ahead, which VERY OFTEN lands just off the mesh (past a
	// wall, over the perimeter, above a carved hole). MoveTo's own projection uses the agent's tiny
	// DefaultQueryExtent (50,50,250) and misses → it returns Failed and the bot FREEZES. So snap the goal to
	// the navmesh ourselves with a generous extent, and only path to a real on-mesh point. (With partial paths
	// on, an unreachable-but-on-mesh goal succeeds; a hard Failed only happens when the goal isn't on the mesh.)
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	FVector Goal = RawGoal;
	bool bGoalOnMesh = false;
	if (Nav != nullptr)
	{
		FNavLocation Projected;
		if (Nav->ProjectPointToNavigation(RawGoal, Projected, FVector(600.f, 600.f, 500.f)))
		{
			Goal = Projected.Location;
			bGoalOnMesh = true;
		}
	}

	FAIMoveRequest Req;
	if (!bGoalOnMesh && FallbackActor != nullptr)
	{
		// No mesh near the desired heading (e.g. facing straight into a wall). Path to the enemy instead — a
		// pawn is always on the mesh — so the bot advances toward the fight rather than standing still.
		Req.SetGoalActor(FallbackActor);
	}
	else
	{
		Req.SetGoalLocation(Goal);
	}
	Req.SetAcceptanceRadius(MoveAcceptUU);
	Req.SetUsePathfinding(true);
	Req.SetAllowPartialPath(true);      // unreachable goal → walk as far along the route as the mesh allows
	Req.SetProjectGoalLocation(true);   // belt+braces (we already snapped Goal above)
	Req.SetCanStrafe(true);             // decouple facing from move dir — the aim code owns yaw
	const FPathFollowingRequestResult Result = MoveTo(Req);
	if (Result.Code == EPathFollowingRequestResult::RequestSuccessful)
	{
		CurrentMoveId = Result.MoveId;
	}
	else if (Result.Code == EPathFollowingRequestResult::Failed)
	{
		// Still failed after snapping the goal. Distinguish "goal genuinely off-mesh" from "navmesh is
		// LOCAL/fragmented" (bot on a small island, can't reach the arena centre) so the log pinpoints it.
		if (NavWarnTimer <= 0.f)
		{
			NavWarnTimer = 3.f;
			const FVector BotLoc = GetPawn() ? GetPawn()->GetActorLocation() : FVector::ZeroVector;
			FNavLocation Here, Ctr;
			const bool bHere = (Nav != nullptr) && Nav->ProjectPointToNavigation(BotLoc, Here, FVector(200.f, 200.f, 400.f));
			const bool bCtr  = (Nav != nullptr) && Nav->ProjectPointToNavigation(FVector(3200.f, 2000.f, BotLoc.Z), Ctr, FVector(800.f, 800.f, 800.f));
			UE_LOG(PaintForgeLog, Warning,
				TEXT("Bot MoveTo FAILED. NavSys=%s BotOnMesh=%s GoalSnapped=%s ArenaCtrOnMesh=%s -> %s"),
				Nav ? TEXT("yes") : TEXT("NULL"), bHere ? TEXT("yes") : TEXT("NO"),
				bGoalOnMesh ? TEXT("yes") : TEXT("NO"), bCtr ? TEXT("yes") : TEXT("NO"),
				(bHere && !bCtr) ? TEXT("navmesh looks LOCAL/fragmented") : TEXT("goal unreachable this beat"));
		}
	}
}

void APFBotController::OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result)
{
	Super::OnMoveCompleted(RequestID, Result);
	// A blocked/invalid finish just means the projected goal was unreachable this beat; the next Tick picks a
	// fresh tactical goal and re-paths. Nothing to do here but let the throttle re-fire (bIdle in Tick catches it).
}

FVector APFBotController::ChooseTacticalPosition(const APaintForgeCharacter* Target) const
{
	const APaintForgeCharacter* Bot = GetBotCharacter();
	UWorld* World = GetWorld();
	if (Bot == nullptr || Target == nullptr || World == nullptr)
	{
		return (Bot != nullptr) ? Bot->GetActorLocation() : FVector::ZeroVector;
	}
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	const FVector BotLoc = Bot->GetActorLocation();
	const FVector EnemyLoc = Target->GetActorLocation();
	const FVector EnemyEye = EnemyLoc + FVector(0.f, 0.f, 40.f);
	const FVector EnemyFwd = FVector(Target->GetActorForwardVector().X, Target->GetActorForwardVector().Y, 0.f).GetSafeNormal();

	// Teammate positions (spread / de-clump). Small: <= a few per side.
	TArray<FVector, TInlineAllocator<8>> Mates;
	const APaintForgePlayerState* MyPS = GetPlayerState<APaintForgePlayerState>();
	if (const APaintForgeGameState* GS = World->GetGameState<APaintForgeGameState>())
	{
		for (APlayerState* PSBase : GS->PlayerArray)
		{
			const APaintForgePlayerState* OtherPS = Cast<APaintForgePlayerState>(PSBase);
			if (OtherPS == nullptr || OtherPS == MyPS || !OtherPS->bAliveInRound || IsHostilePlayerState(OtherPS))
			{
				continue;   // only living teammates
			}
			if (const APawn* MatePawn = OtherPS->GetPawn())
			{
				Mates.Add(MatePawn->GetActorLocation());
			}
		}
	}

	FVector BestPos = BotLoc;
	float BestScore = -TNumericLimits<float>::Max();
	FCollisionQueryParams Q(FName(TEXT("BotTactic")), /*bTraceComplex=*/false, Bot);
	Q.AddIgnoredActor(Target);

	auto Evaluate = [&](const FVector& Raw)
	{
		FVector P = Raw;
		if (Nav != nullptr)
		{
			FNavLocation Proj;
			if (!Nav->ProjectPointToNavigation(Raw, Proj, FVector(220.f, 220.f, 300.f)))
			{
				return;   // not on the navmesh → not a place we can stand
			}
			P = Proj.Location;
		}
		const float Dist = FVector::Dist2D(P, EnemyLoc);
		if (Dist > EngageRangeUU)
		{
			return;   // can't shoot from out here
		}

		float Score = 0.f;

		// (1) Can we SHOOT from here? A firing position needs LOS; no-LOS spots are only retreats → penalised.
		FHitResult Hit;
		const bool bLOS = !World->LineTraceSingleByChannel(Hit, P + FVector(0.f, 0.f, 60.f), EnemyEye, ECC_Visibility, Q);
		Score += bLOS ? 3.f : -2.5f;

		// (2) Good range: prefer the stand-off band; punish being right on top of the enemy.
		Score += 1.f - FMath::Abs(Dist - PreferredRangeUU) / FMath::Max(EngageRangeUU, 1.f);
		if (Dist < MinRangeUU) { Score -= 1.f; }

		// (3) Near cover: short cardinal probes — a wall/prop beside us is good (we can duck); boxed-in is bad.
		int32 CoverSides = 0;
		static const FVector Dirs[4] = { FVector(1,0,0), FVector(-1,0,0), FVector(0,1,0), FVector(0,-1,0) };
		const FVector Chest = P + FVector(0.f, 0.f, 40.f);
		for (const FVector& D : Dirs)
		{
			FHitResult CH;
			if (World->LineTraceSingleByChannel(CH, Chest, Chest + D * CoverProbeUU, ECC_WorldStatic, Q)) { ++CoverSides; }
		}
		Score += CoverSides * 0.5f;
		if (CoverSides >= 4) { Score -= 1.5f; }   // fully walled in — can't fight from here

		// (4) Flank: prefer the enemy's side/rear over walking straight into their facing.
		const FVector EToP = FVector(P.X - EnemyLoc.X, P.Y - EnemyLoc.Y, 0.f).GetSafeNormal();
		if (!EnemyFwd.IsNearlyZero() && !EToP.IsNearlyZero())
		{
			Score += (1.f - FVector::DotProduct(EnemyFwd, EToP)) * 0.5f;   // 0 in front → +1 behind
		}

		// (5) Spread: don't pile onto a teammate's spot.
		for (const FVector& M : Mates)
		{
			if (FVector::DistSquared2D(P, M) < FMath::Square(SpreadRadiusUU)) { Score -= 0.7f; }
		}

		// (6) Inertia: a small bonus for holding ground so bots don't dither between near-equal spots.
		if (FVector::DistSquared2D(P, BotLoc) < FMath::Square(150.f)) { Score += 0.3f; }

		if (Score > BestScore) { BestScore = Score; BestPos = P; }
	};

	Evaluate(BotLoc);   // include "stay put" as a candidate
	for (int32 i = 0; i < 8; ++i)
	{
		const FVector Dir = FRotator(0.f, i * 45.f, 0.f).Vector();
		Evaluate(BotLoc + Dir * ReposSampleNearUU);
		Evaluate(BotLoc + Dir * ReposSampleFarUU);
	}
	return BestPos;
}

bool APFBotController::IsHostilePlayerState(const APaintForgePlayerState* OtherPS) const
{
	const APaintForgePlayerState* MyPS = GetPlayerState<APaintForgePlayerState>();
	if (OtherPS == nullptr || MyPS == nullptr || OtherPS == MyPS)
	{
		return false;
	}
	// Free-for-All has no teams — every other combatant is an enemy. Team modes: different assigned team = enemy.
	const APaintForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<APaintForgeGameState>() : nullptr;
	if (GS != nullptr && GS->MatchType == EPFMatchType::FreeForAll)
	{
		return true;
	}
	if (OtherPS->TeamId > 1)
	{
		return false;   // unassigned
	}
	return OtherPS->TeamId != MyPS->TeamId;
}

ETeamAttitude::Type APFBotController::GetTeamAttitudeTowards(const AActor& Other) const
{
	// Perception's affiliation filter (bDetectEnemies) resolves friend/foe through this. Key off the other
	// pawn's PlayerState team via the game's own rules, so we don't need every player/bot controller to carry a
	// matching FGenericTeamId (the classic "player never gets seen" perception trap).
	const APawn* OtherPawn = Cast<const APawn>(&Other);
	const APaintForgePlayerState* OtherPS = OtherPawn ? OtherPawn->GetPlayerState<APaintForgePlayerState>() : nullptr;
	if (OtherPS == nullptr)
	{
		return ETeamAttitude::Neutral;
	}
	return IsHostilePlayerState(OtherPS) ? ETeamAttitude::Hostile : ETeamAttitude::Friendly;
}

void APFBotController::OnPerceptionStimulus(AActor* Actor, FAIStimulus Stimulus)
{
	// Sight is polled in AcquireNearestEnemy; here we only care about HEARING — a heard hostile (gunfire /
	// footsteps) becomes an "investigate this noise" goal so a flanked bot turns toward it instead of ignoring it.
	if (!Stimulus.WasSuccessfullySensed() || Stimulus.Type != HearingSenseID)
	{
		return;
	}
	const APawn* NoisePawn = Cast<APawn>(Actor);
	const APaintForgePlayerState* OtherPS = NoisePawn ? NoisePawn->GetPlayerState<APaintForgePlayerState>() : nullptr;
	if (!IsHostilePlayerState(OtherPS))
	{
		return;
	}
	InvestigatePos = Stimulus.StimulusLocation;
	InvestigateTime = (GetWorld() != nullptr) ? GetWorld()->GetTimeSeconds() : 0.f;
}

APaintForgeCharacter* APFBotController::AcquireNearestEnemy() const
{
	const APaintForgeCharacter* Bot = GetBotCharacter();
	if (Bot == nullptr)
	{
		return nullptr;
	}
	const FVector BotLoc = Bot->GetActorLocation();

	APaintForgeCharacter* Best = nullptr;
	float BestSq = TNumericLimits<float>::Max();
	auto Consider = [&](APaintForgeCharacter* Cand)
	{
		if (Cand == nullptr)
		{
			return;
		}
		const APaintForgePlayerState* CandPS = Cand->GetPlayerState<APaintForgePlayerState>();
		if (CandPS == nullptr || !CandPS->bAliveInRound || !IsHostilePlayerState(CandPS))
		{
			return;
		}
		const float Sq = FVector::DistSquared(BotLoc, Cand->GetActorLocation());
		if (Sq < BestSq) { BestSq = Sq; Best = Cand; }
	};

	// 1. Hostiles the bot can actually SEE — perception sight cone + range (this is what makes bots flankable).
	if (AIPerception != nullptr)
	{
		TArray<AActor*> Seen;
		AIPerception->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Seen);
		for (AActor* A : Seen)
		{
			Consider(Cast<APaintForgeCharacter>(A));
		}
	}

	// 2. Proximity "sixth sense": any hostile within ProximityAwareUU with LOS, regardless of the view cone — so
	//    someone right next to or behind the bot is still noticed (keeps CQB + the point-blank fix reliable, and
	//    is a safety net if sight-affiliation is ever misconfigured).
	const APaintForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<APaintForgeGameState>() : nullptr;
	if (GS != nullptr)
	{
		const float ProxSq = FMath::Square(ProximityAwareUU);
		for (APlayerState* PSBase : GS->PlayerArray)
		{
			APaintForgePlayerState* OtherPS = Cast<APaintForgePlayerState>(PSBase);
			if (OtherPS == nullptr || !OtherPS->bAliveInRound || !IsHostilePlayerState(OtherPS))
			{
				continue;
			}
			APaintForgeCharacter* OtherChar = Cast<APaintForgeCharacter>(OtherPS->GetPawn());
			if (OtherChar == nullptr)
			{
				continue;
			}
			if (FVector::DistSquared(BotLoc, OtherChar->GetActorLocation()) <= ProxSq && HasLineOfSight(OtherChar))
			{
				Consider(OtherChar);
			}
		}
	}
	return Best;
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
