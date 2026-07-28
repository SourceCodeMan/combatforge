// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "AI/PFBotController.h"

#include "CombatForge.h"
#include "AI/PFSquadSubsystem.h"
#include "Building/PFArenaShell.h"
#include "Building/PFBuildPieceActor.h"   // bots open closed doors when a path stalls
#include "Combat/PFSmokeSubsystem.h"
#include "Player/CombatForgeCharacter.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"
#include "Core/CombatForgeTypes.h"
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
		UE_LOG(CombatForgeLog, Warning,
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
	bWantsPlayerState = true;                       // → ACombatForgePlayerState in PlayerArray (team/alive/spread-seed)
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

ACombatForgeCharacter* APFBotController::GetBotCharacter() const
{
	return Cast<ACombatForgeCharacter>(GetPawn());
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
	LastKnownTotalHits = 255;   SuppressedUntil = -1000.f;   // fresh hit-tracking/suppression for the new life
	ScanTimer = 0.f;   ScanYawOffset = 0.f;   ScanPitchOffset = 0.f;
	JumpStallTimer = 0.f;   JumpCooldown = 0.f;
	LastCombatTime = -1000.f;   // fresh life = out of combat → first contact gets a full reaction delay
	TriggerPullTimer = 0.f;
	// Give this bot a team id so perception has a concrete affiliation (attitude itself comes from the
	// GetTeamAttitudeTowards override, but a real id avoids any NoTeam short-circuit in the sense filter).
	if (const ACombatForgePlayerState* PS = GetPlayerState<ACombatForgePlayerState>())
	{
		SetGenericTeamId(FGenericTeamId(PS->TeamId <= 1 ? PS->TeamId : 2));
	}
	// Field centre from the live shell (map-dependent — see CachedFieldCenter). Re-resolved every
	// possess: a lobby map switch respawns the shell, and bots are re-possessed at Lobby→Build.
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<APFArenaShell> It(World); It; ++It)
		{
			CachedFieldCenter = It->GetFieldCenter();
			break;
		}
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
	// Engage ranges are CROSS-MAP now (arena diagonal ~7500): "if I can see them, they should be shooting me."
	// Skill difficulty comes from aim error / turn rate / reaction, not from refusing to fire at distance.
	switch (Effective)
	{
	// Aim acquire ~25% slower than prior (Tom 2026-07-17: bots felt OP) — turn rate ×0.75, reaction ×1.25.
	// Difficulty still separates mainly on aim error / turn rate; engage ranges stay cross-map.
	case EPFBotSkill::Rookie:
		AimErrorDeg = 14.f;  AimTurnRate = 1.875f;  ReactionDelay = 0.44f;  EngageRangeUU = 7000.f;  break;
	case EPFBotSkill::Sharpshooter:
		AimErrorDeg = 1.5f;  AimTurnRate = 8.25f;   ReactionDelay = 0.09f;  EngageRangeUU = 8500.f;  break;
	case EPFBotSkill::Regular:
	default:
		AimErrorDeg = 4.5f;  AimTurnRate = 4.875f;  ReactionDelay = 0.20f;  EngageRangeUU = 8000.f;  break;
	}
}

void APFBotController::UpdateControlRotation(float DeltaTime, bool bUpdatePawn)
{
	// Copy of AAIController::UpdateControlRotation with the pitch-zeroing REMOVED. The engine base
	// (Engine/Source/Runtime/AIModule/Private/AIController.cpp:444-448, "Don't pitch view unless looking at
	// another pawn") force-zeroes control-rotation PITCH every tick whenever no focus ACTOR is set — and it
	// runs from AAIController::Tick, i.e. via our Super::Tick BEFORE the aim code below. We aim by
	// SetControlRotation (including pitch at elevated/low targets) and never SetFocus, so the base zeroed our
	// pitch every frame: RInterpTo restarted from 0 each tick and never converged, so bot shots came out
	// nearly level ("perfectly level splat line") and bots literally could not shoot up or down. Keep the
	// pitch our aim set; the pawn body still faces our YAW via FaceRotation (which respects the character's
	// bUseControllerRotationPitch=false, so the body stays upright while control rotation carries the pitch
	// purely as the shot direction).
	APawn* const MyPawn = GetPawn();
	if (MyPawn == nullptr)
	{
		return;
	}
	FRotator NewControlRotation = GetControlRotation();
	if (bSetControlRotationFromPawnOrientation)   // false for us; kept faithful to the base in case it flips
	{
		NewControlRotation = MyPawn->GetActorRotation();
	}
	SetControlRotation(NewControlRotation);
	if (bUpdatePawn && !MyPawn->GetActorRotation().Equals(NewControlRotation, 1e-3f))
	{
		MyPawn->FaceRotation(NewControlRotation, DeltaTime);
	}
}

void APFBotController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	ACombatForgeCharacter* Bot = GetBotCharacter();
	ACombatForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
	if (Bot == nullptr || GS == nullptr)
	{
		return;
	}
	const ACombatForgePlayerState* PS = GetPlayerState<ACombatForgePlayerState>();

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

	// Suppression: detect taking a hit (TotalHits rose since last tick) → mark "under fire" (widens aim
	// below) and, on a fresh hit, break for a new tactical position (cover). Keyed off TotalHits, NOT a
	// derived remaining value: a limb hit can leave the nearest-threshold remainder unchanged and the
	// bot would never notice being shot.
	if (Health != nullptr)
	{
		if (LastKnownTotalHits != 255 && Health->TotalHits > LastKnownTotalHits)
		{
			const bool bWasSuppressed = (NowSec < SuppressedUntil);
			SuppressedUntil = NowSec + SuppressDurationSec;
			if (!bWasSuppressed) { bHaveTacticalGoal = false; }   // just got shot → reposition to cover now
		}
		LastKnownTotalHits = Health->TotalHits;
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
		ACombatForgeCharacter* Curr = CurrentTarget.Get();
		const bool bCurrOK = IsTargetEngageable(Curr);
		if (bCurrOK)
		{
			LastCombatTime = NowSec;   // we have live contact — keep the combat window open
		}
		ACombatForgeCharacter* Best = AcquireNearestEnemy();   // nearest VISIBLE enemy (else nearest)
		if (Best != nullptr && Best != Curr)
		{
			const FVector BotAt = Bot->GetActorLocation();
			const float BestD = FVector::Dist(BotAt, Best->GetActorLocation());
			const float CurrD = bCurrOK ? FVector::Dist(BotAt, Curr->GetActorLocation()) : TNumericLimits<float>::Max();
			// Switch if: no valid target, OR the new one is clearly closer (hysteresis), OR it's a point-blank
			// threat while the current target is NOT already close. That last guard is key: two clustered close
			// enemies must NOT make the bot switch every refresh — a bot on a FAR foe still turns to a point-blank
			// attacker, but it won't ping-pong between two enemies already in its face.
			const bool bClearlyCloser = BestD < CurrD * 0.8f;
			const bool bPointBlankThreat = (BestD < PointBlankUU) && (CurrD > PointBlankUU);
			// TRADE UP TO A KILLABLE TARGET: acquisition/retention run on EYE line-of-sight, so a bot can stay
			// locked on an enemy it can SEE but not HIT (head over low cover, muzzle blocked) while ignoring a
			// closer, fully-exposed one — the "kept shooting my covered teammates, never turned to shoot me"
			// bug. If the current target isn't cleanly shootable (muzzle LOS) and the new one IS, switch even
			// inside the distance hysteresis. Both false / both true falls back to the distance rules below.
			const bool bCurrHittable = bCurrOK && HasLineOfSight(Curr, /*bBodiesBlock=*/true);
			const bool bTradeUpToHittable = !bCurrHittable && HasLineOfSight(Best, /*bBodiesBlock=*/true);
			if (!bCurrOK || bClearlyCloser || bPointBlankThreat || bTradeUpToHittable)
			{
				// Re-arm the "notice" reaction gap ONLY when the bot has genuinely been OUT of combat for
				// CombatMemorySec. Re-arming on any lesser condition (every switch, or even every acquire-from-
				// invalid) left crowds of bots perpetually "noticing": momentary target losses re-armed the gap
				// faster than it could count down, so whole clusters stood staring (playtest 07-14, twice).
				if ((NowSec - LastCombatTime) > CombatMemorySec)
				{
					FireHoldTimer = (BestD < PointBlankUU) ? (ReactionDelay * 0.35f) : ReactionDelay;
				}
				CurrentTarget = Best;
				bHaveTacticalGoal = false;   // new target → re-pick a firing position now, don't reuse the old one
			}
			LastCombatTime = NowSec;   // acquiring/holding an enemy counts as contact either way
		}
		else if (Best == nullptr && !bCurrOK)
		{
			CurrentTarget = nullptr;
		}
	}

	ACombatForgeCharacter* Target = CurrentTarget.Get();

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

	// Human-like scanning: when NOT engaged, glance around — mostly sweep the front arc, periodically snap a
	// flank/rear check — so the view cone eventually covers behind the bot and a patient flanker is caught. The
	// offset rides on top of whatever the bot is looking toward (goal/search). Reset the instant we have a target.
	if (Target != nullptr)
	{
		ScanYawOffset = 0.f;
	}
	else
	{
		ScanTimer -= DeltaSeconds;
		if (ScanTimer <= 0.f)
		{
			ScanYawOffset = (FMath::FRand() < 0.30f)
				? FMath::FRandRange(120.f, 200.f) * (FMath::FRand() < 0.5f ? -1.f : 1.f)   // glance to a flank / behind
				: FMath::FRandRange(-70.f, 70.f);                                          // sweep the front arc
			// Verticality: a third of glances check HIGH (upper floors, wall tops), some check low; the sight
			// cone follows the look pitch, so bots actually notice players holding high ground now.
			const float R = FMath::FRand();
			ScanPitchOffset = (R < 0.35f) ? FMath::FRandRange(15.f, 40.f)
			                 : (R < 0.50f) ? FMath::FRandRange(-25.f, -10.f)
			                 : FMath::FRandRange(-5.f, 8.f);
			ScanTimer = FMath::FRandRange(1.2f, 2.6f);
		}
	}

	// Objective goal (Domination / Hardpoint / CTF): where this bot should push, even with no enemy in sight.
	FVector ObjGoal;
	const bool bHasObjective = ComputeObjectiveGoal(ObjGoal);
	// Hold-ground modes (Domination / Hardpoint): the bot should CONTEST its point — fight its way to it and hold
	// it — instead of letting a spotted enemy draw it off the objective. (CTF is a carry mode, so it's excluded.)
	const bool bHoldGround = bHasObjective
		&& (GS->MatchType == EPFMatchType::Domination || GS->MatchType == EPFMatchType::Hardpoint);

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
				// De-clump: every no-target bot gets the SAME lead, so without an offset they all pile onto one
				// spot, stand shoulder-to-shoulder, and block each other. Approach from a per-bot angle instead.
				Lead += FRotator(0.f, PS->RosterIndex * 137.f, 0.f).Vector() * 350.f;
				SearchPos = Lead;
				bHaveSearch = true;
			}
		}
	}

	// Consume a REACHED search point: arriving and finding nothing means the lead is spent — clear it so the
	// bot flows on to the objective / seek-contact instead of camping the spot staring at a wall (playtest:
	// pairs of bots standing idle at walls were parked on stale investigate points, constantly refreshed by
	// distant gunfire noise).
	if (bHaveSearch && Target == nullptr && FVector::Dist2D(Bot->GetActorLocation(), SearchPos) < 250.f)
	{
		LastSeenTime = -1000.f;
		InvestigateTime = -1000.f;
		bHaveSearch = false;
	}

	const FVector BotLoc = Bot->GetActorLocation();

	// Nothing to fight, push, or investigate → SEEK CONTACT: push toward the arena centre (where fights happen)
	// while panning the view, rather than standing at spawn until an enemy wanders into the sight cone. Since
	// perception only reports enemies the bot can actually see/sense, without this a Skirmish/FFA bot with no one
	// in view would freeze. Hold + slow-scan once at the centre.
	if (Target == nullptr && !bHasObjective && !bHaveSearch)
	{
		SetFiring(false);
		const FVector ArenaCenter(CachedFieldCenter.X, CachedFieldCenter.Y, BotLoc.Z);
		if (FVector::Dist2D(BotLoc, ArenaCenter) > 800.f)
		{
			const FVector ToC = ArenaCenter - (BotLoc + FVector(0.f, 0.f, 60.f));
			if (!ToC.IsNearlyZero())
			{
				FRotator LookRot = ToC.Rotation();
				LookRot.Yaw += ScanYawOffset;     // glance around while advancing
				LookRot.Pitch = FMath::Clamp(LookRot.Pitch + ScanPitchOffset, -60.f, 60.f);   // check high ground too
				SetControlRotation(FMath::RInterpTo(GetControlRotation(), LookRot, DeltaSeconds, 3.5f));
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
		const float HealthFrac = (Health != nullptr && Health->TotalOut > 0)
			? 1.f - static_cast<float>(Health->TotalHits) / static_cast<float>(Health->TotalOut) : 1.f;
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

		// Firing check is the STRICT one (bBodiesBlock): geometry blocks, a friendly in the line of fire holds the
		// trigger, but a hostile in the way is fine to shoot. bAlongCurrentAim traces the ACTUAL BB path (the aim
		// is on Target here) so the bot won't fire when its rounds would splat on cover it can see the enemy over.
		// Target RETENTION elsewhere stays body-transparent.
		const bool bLOS = HasLineOfSight(Target, /*bBodiesBlock=*/true, /*bAlongCurrentAim=*/true);
		const bool bWantFire = (Dist <= EngageRangeUU) && (FireHoldTimer <= 0.f) && bLOS;
		SetFiring(bWantFire);
		if (bWantFire)
		{
			LastCombatTime = NowSec;   // actively shooting = definitely in combat

			// RE-PULL the trigger for Single/Burst bots. SetFiring only presses on the edge, and a Single-mode
			// weapon fires ONE shot per pull — so a third of the roster (mode variety is per-bot) fired once per
			// engagement and then stood there while the Auto bots did all the shooting ("only one bot shoots at
			// a time"). Cycle the trigger at a humanlike cadence while the bot still wants to fire.
			if (UPFWeaponComponent* Weapon = Bot->GetWeapon())
			{
				if (Weapon->GetFireMode() != EPFFireMode::Auto)
				{
					TriggerPullTimer -= DeltaSeconds;
					if (TriggerPullTimer <= 0.f)
					{
						Weapon->StopFire();
						Weapon->StartFire();
						TriggerPullTimer = (Weapon->GetFireMode() == EPFFireMode::Single) ? 0.45f : 0.85f;
					}
				}
			}
		}

		// Diagnostic: point-blank enemy, reaction gap ALREADY expired, still not firing → genuine stall worth a
		// log line. (A counting-down FireHoldTimer is healthy and no longer logged — pre-fix it spammed hundreds
		// of lines a match and buried the real signal.) Throttled (shares NavWarnTimer).
		if (!bWantFire && Dist < PointBlankUU && FireHoldTimer <= 0.f && NavWarnTimer <= 0.f)
		{
			NavWarnTimer = 3.f;
			UE_LOG(CombatForgeLog, Warning,
				TEXT("Bot NOT firing point-blank (stall): dist=%.0f LOS=%d inRange=%d"),
				Dist, bLOS ? 1 : 0, (Dist <= EngageRangeUU) ? 1 : 0);
		}
	}
	else
	{
		// No target: hold fire and look toward where we're heading (search LKP / objective) WITH scan glances on
		// top, so the sight cone sweeps its surroundings — a bot hunting your last-known-position also checks its
		// flanks and back rather than tunnel-visioning straight ahead.
		SetFiring(false);
		const FVector LookAt = bHaveSearch ? SearchPos : ObjGoal;
		const FVector ToLook = LookAt - (BotLoc + FVector(0.f, 0.f, 60.f));
		if (!ToLook.IsNearlyZero())
		{
			FRotator LookRot = ToLook.Rotation();
			LookRot.Yaw += ScanYawOffset;
			LookRot.Pitch = FMath::Clamp(LookRot.Pitch + ScanPitchOffset, -60.f, 60.f);   // sweep upper levels too
			SetControlRotation(FMath::RInterpTo(GetControlRotation(), LookRot, DeltaSeconds, 3.5f));
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
	if (Target != nullptr && bHoldGround && FVector::Dist2D(BotLoc, ObjGoal) > ObjectiveHoldRadiusUU * 2.f)
	{
		// FIGHT TOWARD THE OBJECTIVE: we can see an enemy but we're off our point — advance to contest it while
		// still shooting (aim/fire above tracks the enemy), rather than chasing them away from the objective.
		GoalLoc = ObjGoal;
		bMove = true;
	}
	else if (Target != nullptr)
	{
		// FIGHT: reposition to the best nearby firing position (LOS + cover + range + flank + spread), re-evaluated
		// periodically. This replaces the old strafe-in-the-open with deliberate tactical movement — bots work to
		// cover, hold angles, and flank as a group. In hold-ground modes the position is anchored to the objective
		// so the bot fights FROM the point. Aim/fire (above) keeps the enemy tracked while we move.
		ReposTimer -= DeltaSeconds;
		if (!bHaveTacticalGoal || ReposTimer <= 0.f)
		{
			TacticalGoal = ChooseTacticalPosition(Target, bHoldGround ? &ObjGoal : nullptr);
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

		// JUMP low barriers: the navmesh can't route over knee/waist walls, so bots were confined to ground
		// lanes players hop over freely. A bot stalled against a JUMPABLE obstacle (knee trace blocked, head
		// trace clear) hops it and keeps pushing toward its goal — verticality via the same moves players make.
		JumpCooldown -= DeltaSeconds;
		const float Speed2D = Bot->GetVelocity().Size2D();
		JumpStallTimer = (Speed2D < 60.f) ? (JumpStallTimer + DeltaSeconds) : 0.f;

		// OPEN DOORS (Tom 2026-07-18): a closed player-built door is a hard navmesh obstacle, so bots used to
		// stall against one and route the long way (or fail). When stalled, open any closed door within reach.
		// The bot Tick runs on the host (authority), so we can call the door API directly; AuthorityTryToggleDoor
		// re-validates range + a one-way door's front face, so a wrong-side approach simply no-ops and the bot
		// routes on. Throttled, and only on a stall, so it's cheap.
		DoorTryCooldown -= DeltaSeconds;
		if (DoorTryCooldown <= 0.f && JumpStallTimer > 0.3f)
		{
			DoorTryCooldown = 0.5f;
			if (UWorld* DW = GetWorld())
			{
				const float DoorRangeSq = FMath::Square(APFBuildPieceActor::DoorInteractRangeUU);
				for (TActorIterator<APFBuildPieceActor> It(DW); It; ++It)
				{
					APFBuildPieceActor* Piece = *It;
					if (Piece == nullptr || Piece->IsOpen())
					{
						continue;
					}
					// NORMAL doors only. Bots must never touch a one-way door: a stalled front-side bot
					// opened it, the 3s auto-close shut it, and the still-stalled bot re-opened on its
					// next 0.5s cooldown — an endless flicker (this loop skips OPEN doors, so bots only
					// ever open). Each re-close of a SEALED one-way snapped straight to the wall plate,
					// which players saw as "flashes open and turns into a wall" right after sealing
					// (Tom, alpha-9). One-ways also still carve the navmesh, so bots path around them.
					const EPFPieceType PT = Piece->GetPieceType();
					if (PT != EPFPieceType::WallDoor)
					{
						continue;
					}
					if (FVector::DistSquared(Piece->GetDoorInteractLocation(), BotLoc) <= DoorRangeSq)
					{
						Piece->AuthorityTryToggleDoor(Bot);   // opens if the face/range check passes; harmless no-op otherwise
					}
				}
			}
		}

		if (JumpCooldown <= 0.f && JumpStallTimer > 0.4f)
		{
			if (UWorld* JW = GetWorld())
			{
				const FVector Fwd = (GoalLoc - BotLoc).GetSafeNormal2D();
				if (!Fwd.IsNearlyZero())
				{
					FCollisionQueryParams JQ(FName(TEXT("BotJump")), /*bTraceComplex=*/false, Bot);
					FHitResult KneeHit, HeadHit;
					const FVector KneeStart = BotLoc + FVector(0.f, 0.f, -50.f);
					const bool bKneeBlocked = JW->LineTraceSingleByChannel(KneeHit, KneeStart, KneeStart + Fwd * 120.f, ECC_WorldStatic, JQ);
					const FVector HeadStart = BotLoc + FVector(0.f, 0.f, 70.f);
					const bool bHeadBlocked = JW->LineTraceSingleByChannel(HeadHit, HeadStart, HeadStart + Fwd * 160.f, ECC_WorldStatic, JQ);
					if (bKneeBlocked && !bHeadBlocked)
					{
						Bot->Jump();
						Bot->AddMovementInput(Fwd, 1.f);   // carry over the lip while airborne
						JumpCooldown = 1.2f;
						JumpStallTimer = 0.f;
					}
				}
			}
		}
	}
	else
	{
		StopMovement();
		JumpStallTimer = 0.f;
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
		// No mesh near the desired heading (e.g. facing straight into a wall). Path toward the enemy, but
		// NEVER use SetGoalActor on a live pawn — PathFollowing will try to reach the capsule center and
		// the overlap depenetration is what launches bots into the air. Aim at a point standoff away.
		const FVector BotHere = GetPawn() ? GetPawn()->GetActorLocation() : Goal;
		const FVector EnemyHere = FallbackActor->GetActorLocation();
		FVector Away = (BotHere - EnemyHere).GetSafeNormal2D();
		if (Away.IsNearlyZero())
		{
			Away = FVector(1.f, 0.f, 0.f);
		}
		FVector Standoff = EnemyHere + Away * FMath::Max(BotBodyClearanceUU, MoveAcceptUU);
		if (Nav != nullptr)
		{
			FNavLocation Proj;
			if (Nav->ProjectPointToNavigation(Standoff, Proj, FVector(600.f, 600.f, 500.f)))
			{
				Standoff = Proj.Location;
			}
		}
		Req.SetGoalLocation(Standoff);
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
			const bool bCtr  = (Nav != nullptr) && Nav->ProjectPointToNavigation(FVector(CachedFieldCenter.X, CachedFieldCenter.Y, BotLoc.Z), Ctr, FVector(800.f, 800.f, 800.f));
			UE_LOG(CombatForgeLog, Warning,
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

FVector APFBotController::ChooseTacticalPosition(const ACombatForgeCharacter* Target, const FVector* Anchor) const
{
	const ACombatForgeCharacter* Bot = GetBotCharacter();
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
	const ACombatForgePlayerState* MyPS = GetPlayerState<ACombatForgePlayerState>();
	if (const ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>())
	{
		for (APlayerState* PSBase : GS->PlayerArray)
		{
			const ACombatForgePlayerState* OtherPS = Cast<ACombatForgePlayerState>(PSBase);
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

	// Score the firing-position LOS from roughly the bot's MUZZLE height, not the eye — the fire gate traces
	// from GetMuzzleLocation(false) (lower than the +60 eye), so an eye-only score picked spots where the eye
	// clears a wall lip but the leveled barrel does not, and the bot then held a position it couldn't shoot
	// from. Constant offset sampled at the current pose (per-candidate muzzle would need a pose the bot isn't in).
	const float MuzzleProbeZ = FMath::Clamp(Bot->GetMuzzleLocation(false).Z - BotLoc.Z, 20.f, 60.f);

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
		// HARD REJECT body-contact spots. The LOS term below is worth a 5.5-point swing, which used to dwarf the
		// old -0.75 "don't stand inside them" nudge — so whenever nearby cover lacked LOS, the WINNING firing
		// position was effectively the enemy's own capsule. Bots then pressed into each other at full acceleration,
		// and the resulting capsule overlap got resolved by a vertical depenetration teleport = the "bots launch to
		// the roof" bug. Two capsule radii is 68uu; 150 keeps a real body gap even with path-follow overshoot.
		if (Dist < BotBodyClearanceUU)
		{
			return;
		}

		float Score = 0.f;

		// (1) Can we SHOOT from here? A firing position needs LOS; no-LOS spots are only retreats → penalised.
		//     Trace from muzzle height (see MuzzleProbeZ) so a "good" spot can actually land a shot.
		FHitResult Hit;
		const bool bLOS = !World->LineTraceSingleByChannel(Hit, P + FVector(0.f, 0.f, MuzzleProbeZ), EnemyEye, ECC_Visibility, Q);
		Score += bLOS ? 3.f : -2.5f;

		// AGGRESSION MODEL (playtest: "bots happy to stay behind cover on their side"): an UNHURT bot presses
		// the attack — positions CLOSER to the enemy score higher and cover is nearly ignored. Only a bot that
		// has actually been HIT (suppressed window) values cover and stand-off range. Shooting well was never
		// the problem; camping was.
		const UWorld* W2 = GetWorld();
		const bool bHurt = (W2 != nullptr) && (static_cast<float>(W2->GetTimeSeconds()) < SuppressedUntil);

		if (bHurt)
		{
			// (2a) Hurt: prefer the stand-off band while the sting wears off.
			Score += 1.f - FMath::Abs(Dist - PreferredRangeUU) / FMath::Max(EngageRangeUU, 1.f);
			if (Dist < MinRangeUU) { Score -= 1.f; }
		}
		else
		{
			// (2b) Unhurt: ADVANCE. Closer beats farther, down to a bayonet-range floor.
			Score += 1.5f * (1.f - FMath::Clamp(Dist / FMath::Max(EngageRangeUU, 1.f), 0.f, 1.f));
			if (Dist < MinRangeUU * 0.5f) { Score -= 0.75f; }   // don't literally stand inside them
		}

		// (3) Near cover: short cardinal probes. Full weight only while HURT; a healthy bot barely cares.
		int32 CoverSides = 0;
		static const FVector Dirs[4] = { FVector(1,0,0), FVector(-1,0,0), FVector(0,1,0), FVector(0,-1,0) };
		const FVector Chest = P + FVector(0.f, 0.f, 40.f);
		for (const FVector& D : Dirs)
		{
			FHitResult CH;
			if (World->LineTraceSingleByChannel(CH, Chest, Chest + D * CoverProbeUU, ECC_WorldStatic, Q)) { ++CoverSides; }
		}
		Score += CoverSides * (bHurt ? 0.8f : 0.1f);
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

		// (7) Objective anchor (Hardpoint/Domination): strongly prefer firing from ON/NEAR the point so the bot
		//     contests it while fighting instead of chasing the enemy off the objective.
		if (Anchor != nullptr)
		{
			const float AnchorDist = FVector::Dist2D(P, *Anchor);
			Score += FMath::Clamp(1.5f - AnchorDist / 800.f, -1.5f, 1.5f);   // ~+1.5 on the point → negative far off it
		}

		if (Score > BestScore) { BestScore = Score; BestPos = P; }
	};

	Evaluate(BotLoc);   // include "stay put" as a candidate
	for (int32 i = 0; i < 8; ++i)
	{
		const FVector Dir = FRotator(0.f, i * 45.f, 0.f).Vector();
		Evaluate(BotLoc + Dir * ReposSampleNearUU);
		Evaluate(BotLoc + Dir * ReposSampleFarUU);
	}

	// VERTICAL PURSUIT: the ring above is all horizontal offsets from the bot, so ProjectPointToNavigation
	// always snaps back to the bot's OWN floor — a bot below an enemy on an upper level never sampled a spot
	// up there and just parked below. Seed candidates at the ENEMY's elevation (its footing + a ring on its
	// level) so ProjectPointToNavigation lands them on the enemy's floor; MoveToGoal then paths UP the ramp
	// that connects the levels (and the knee/head jump code carries a lip). These go through the same scorer,
	// so cover/flank/spread still apply — they're just reachable options the bot never had before.
	// NOTE: the enemy's OWN location used to be evaluated here as a candidate standing spot. It is deliberately
	// gone — it asked bots to walk inside another pawn's capsule, which is what produced the vertical
	// depenetration "launch to the roof". The ring below (ReposSampleNearUU = 350uu) still seeds the enemy's
	// ELEVATION, which is all vertical pursuit actually needed.
	for (int32 i = 0; i < 8; ++i)
	{
		const FVector Dir = FRotator(0.f, i * 45.f, 0.f).Vector();
		Evaluate(EnemyLoc + Dir * ReposSampleNearUU);
	}
	return BestPos;
}

bool APFBotController::IsHostilePlayerState(const ACombatForgePlayerState* OtherPS) const
{
	const ACombatForgePlayerState* MyPS = GetPlayerState<ACombatForgePlayerState>();
	if (OtherPS == nullptr || MyPS == nullptr || OtherPS == MyPS)
	{
		return false;
	}
	// Free-for-All has no teams — every other combatant is an enemy. Team modes: different assigned team = enemy.
	const ACombatForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
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
	const ACombatForgePlayerState* OtherPS = OtherPawn ? OtherPawn->GetPlayerState<ACombatForgePlayerState>() : nullptr;
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
	const ACombatForgePlayerState* OtherPS = NoisePawn ? NoisePawn->GetPlayerState<ACombatForgePlayerState>() : nullptr;
	if (!IsHostilePlayerState(OtherPS))
	{
		return;
	}
	InvestigatePos = Stimulus.StimulusLocation;
	InvestigateTime = (GetWorld() != nullptr) ? GetWorld()->GetTimeSeconds() : 0.f;
}

ACombatForgeCharacter* APFBotController::AcquireNearestEnemy() const
{
	// A bot only acquires an enemy it can actually SEE: within sight range, INSIDE its view cone (relative to
	// where it's currently looking — which includes the scan glances), and with a clear line of sight. Anyone
	// outside the cone (e.g. sneaking up behind) is IGNORED until the bot's sweep brings them into view or it
	// hears them. Deliberately a manual cone test rather than the perception sense: exact + tunable FOV, keyed
	// straight off the game's team rules, and independent of perception affiliation config.
	const ACombatForgeCharacter* Bot = GetBotCharacter();
	const ACombatForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
	if (Bot == nullptr || GS == nullptr)
	{
		return nullptr;
	}
	const FVector BotLoc = Bot->GetActorLocation();
	const FVector Look = GetControlRotation().Vector();
	const FVector Look2D = FVector(Look.X, Look.Y, 0.f).GetSafeNormal();
	const float CosHalfFOV = FMath::Cos(FMath::DegreesToRadians(SightFOVHalfDeg));
	const float SightSq = FMath::Square(SightRadiusUU);

	ACombatForgeCharacter* Best = nullptr;
	float BestSq = TNumericLimits<float>::Max();
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* OtherPS = Cast<ACombatForgePlayerState>(PSBase);
		if (OtherPS == nullptr || !OtherPS->bAliveInRound || !IsHostilePlayerState(OtherPS))
		{
			continue;
		}
		ACombatForgeCharacter* OtherChar = Cast<ACombatForgeCharacter>(OtherPS->GetPawn());
		if (OtherChar == nullptr)
		{
			continue;
		}
		// bAliveInRound never clears in the CONTINUOUS modes (Skirmish/CTF/Dom/HP) — an eliminated pawn
		// waiting out its respawn timer still reads alive there, so bots locked on and shot the hidden
		// corpse at the death spot. The health flag is the cross-mode truth (issue #17 AI1).
		if (const UPFHealthComponent* OtherHealth = OtherChar->GetHealth())
		{
			if (OtherHealth->bEliminated)
			{
				continue;
			}
		}
		const FVector ToEnemy = OtherChar->GetActorLocation() - BotLoc;
		const float DistSq = ToEnemy.SizeSquared();
		if (DistSq > SightSq)
		{
			continue;   // out of sight range
		}
		const FVector ToEnemy2D = FVector(ToEnemy.X, ToEnemy.Y, 0.f).GetSafeNormal();
		// The documented close-range "sixth sense": inside ProximityAwareUU the cone test is waived (LOS
		// below still required), so a point-blank attacker behind the bot registers. The property was
		// declared + advertised in three comments but read NOWHERE after the manual-cone rewrite — bots
		// were fully deaf-blind at contact range from behind (issue #17 AI2).
		if (DistSq > FMath::Square(ProximityAwareUU)
			&& !Look2D.IsNearlyZero() && FVector::DotProduct(Look2D, ToEnemy2D) < CosHalfFOV)
		{
			continue;   // outside the view cone (and not close enough to feel) → flankable
		}
		if (!HasLineOfSight(OtherChar))
		{
			continue;   // geometry in the way → not seen
		}
		if (DistSq < BestSq) { BestSq = DistSq; Best = OtherChar; }
	}
	return Best;
}

bool APFBotController::IsTargetEngageable(const ACombatForgeCharacter* Target) const
{
	if (Target == nullptr)
	{
		return false;
	}
	const ACombatForgePlayerState* TargetPS = Target->GetPlayerState<ACombatForgePlayerState>();
	if (TargetPS == nullptr || !TargetPS->bAliveInRound)
	{
		return false;   // dead / despawned → drop it and re-scan
	}
	// Same eliminated gate as acquisition: bAliveInRound stays true in continuous modes, so without this
	// the sticky-retention path kept a bot shooting a respawn-waiting corpse (issue #17 AI1).
	if (const UPFHealthComponent* TargetHealth = Target->GetHealth())
	{
		if (TargetHealth->bEliminated)
		{
			return false;
		}
	}
	const ACombatForgeCharacter* Bot = GetBotCharacter();
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
	const ACombatForgeGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACombatForgeGameState>() : nullptr;
	const ACombatForgePlayerState* MyPS = GetPlayerState<ACombatForgePlayerState>();
	const ACombatForgeCharacter* Bot = GetBotCharacter();
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
		const FVector MyHome = HomeFlag ? HomeFlag->GetHomeLocation()
			: PFObjectiveLayout::FlagHome(MyTeam, PFGetArenaMapDef(GS->ArenaMap).CellsY);
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

	// Hardpoint: one active hill — everyone fights for it (attack AND defense: standing on the owned hill
	// denies the enemy sole occupancy).
	// Domination (CoD model): ALL zones live. Push zones my team does NOT own (nearest first); when more than
	// one needs taking, split the roster by parity so the whole team doesn't stack a single flag. All owned →
	// defend the nearest one.
	TArray<APFControlPointActor*, TInlineAllocator<4>> Capturable;
	APFControlPointActor* NearestAny = nullptr;
	float NearestAnySq = TNumericLimits<float>::Max();
	for (const TWeakObjectPtr<APFControlPointActor>& CPPtr : ControlPointsCache)
	{
		APFControlPointActor* CP = CPPtr.Get();
		if (CP == nullptr || !CP->IsPointActive()) { continue; }
		const float DSq = FVector::DistSquared(BotLoc, CP->GetActorLocation());
		if (DSq < NearestAnySq) { NearestAnySq = DSq; NearestAny = CP; }
		if (Mode == EPFMatchType::Domination && CP->GetControllingTeam() != MyTeam)
		{
			Capturable.Add(CP);
		}
	}
	APFControlPointActor* GoalCP = NearestAny;
	if (Mode == EPFMatchType::Domination && Capturable.Num() > 0)
	{
		Capturable.Sort([&BotLoc](const APFControlPointActor& L, const APFControlPointActor& R)
		{
			return FVector::DistSquared(BotLoc, L.GetActorLocation())
				< FVector::DistSquared(BotLoc, R.GetActorLocation());
		});
		const int32 Pick = (Capturable.Num() > 1 && (MyPS->RosterIndex % 2) == 1) ? 1 : 0;
		GoalCP = Capturable[Pick];
	}
	if (GoalCP != nullptr)
	{
		OutGoal = GoalCP->GetActorLocation();
		return true;
	}
	return false;
}

bool APFBotController::HasLineOfSight(const ACombatForgeCharacter* Target, bool bBodiesBlock, bool bAlongCurrentAim) const
{
	const ACombatForgeCharacter* Bot = GetBotCharacter();
	UWorld* World = GetWorld();
	if (Bot == nullptr || Target == nullptr || World == nullptr)
	{
		return false;
	}
	// FIRING check traces from the MUZZLE, not the eye: bots stood "shooting at walls" (and whole hardpoint
	// scrums traded fire without ever killing) because the eye cleared a half-height wall while the leveled
	// barrel below it didn't — every BB ate the wall lip. If the barrel can't clear, don't fire; the tactical
	// repositioning then finds a spot where it can. Tracking/acquisition keeps the eye line.
	const FVector Start = bBodiesBlock
		? Bot->GetMuzzleLocation(false)
		: Bot->GetActorLocation() + FVector(0.f, 0.f, 60.f);
	const FVector TargetChest = Target->GetActorLocation() + FVector(0.f, 0.f, 40.f);
	FVector End = TargetChest;
	if (bAlongCurrentAim)
	{
		// FIRE-GATE check: trace where the BB ACTUALLY goes, not an idealized muzzle→chest line. The real ball
		// (PFWeaponComponent::FireOneShot) spawns at the muzzle and CONVERGES on the bot's eye-aim point — it
		// does NOT fly parallel to control rotation. Mirror that convergence (eye ray → hit, clamped ahead of
		// the muzzle, matching PFConvergedShotDir) so the gate tests the true shot path: a flat muzzle ray
		// would pass while the real shot — rising from the lower muzzle toward the eye line — clips an overhang
		// (or vice versa). Only valid when control rotation IS aimed at Target (the fire gate).
		const FVector Eye = Bot->GetEyeWorldLocation();
		const FVector AimFwd = Bot->GetBaseAimRotation().Vector();
		FVector Converge = Eye + AimFwd * 100000.f;
		FHitResult EyeHit;
		FCollisionQueryParams EyeQ(FName(TEXT("BotConverge")), /*bTraceComplex=*/false, Bot);
		if (World->LineTraceSingleByChannel(EyeHit, Eye, Converge, ECC_Visibility, EyeQ))
		{
			Converge = EyeHit.ImpactPoint;
		}
		if (((Converge - Start) | AimFwd) < 120.f)   // clamp min convergence, matching the weapon
		{
			Converge = Start + AimFwd * 120.f;
		}
		End = Converge;
	}

	// Smoke conceals: a live smoke cloud on the line breaks sight exactly like world geometry (the bot then
	// falls back to last-known-position hunting — pop smoke and RUN, it works on bots now).
	if (const UPFSmokeSubsystem* Smoke = World->GetSubsystem<UPFSmokeSubsystem>())
	{
		if (Smoke->IsSegmentSmoked(Start, End))
		{
			return false;
		}
	}

	FCollisionQueryParams Params(FName(TEXT("BotLOS")), /*bTraceComplex=*/false, Bot);
	Params.AddIgnoredActor(Target);
	FHitResult Hit;
	const bool bBlocked = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
	if (!bBlocked)
	{
		return true;
	}
	// WHAT blocked matters. In a crowd, character bodies constantly cross the trace — if that counted as
	// "lost sight", every bot's target flickered un-engageable several times a second, each re-acquire re-armed
	// the reaction delay, and whole clusters just stood staring (playtest: "one bot shoots, the rest stand").
	//  - blocked by a HOSTILE body: still a firefight — shooting means you hit that enemy instead. Sight holds.
	//  - blocked by a FRIENDLY body: sight holds for tracking, but bBodiesBlock (the FIRING check) returns
	//    false so a bot never sprays a teammate in the back. Only world geometry truly breaks sight.
	const ACombatForgeCharacter* Blocker = Cast<ACombatForgeCharacter>(Hit.GetActor());
	if (Blocker == nullptr)
	{
		return false;   // wall / built piece / prop — genuinely no line of sight
	}
	if (bBodiesBlock)
	{
		// Firing check: a hostile in the way is a fine thing to shoot; a teammate is not.
		return IsHostilePlayerState(Blocker->GetPlayerState<ACombatForgePlayerState>());
	}
	return true;   // tracking/retention: a body crossing the line never makes the bot "forget" its target
}

void APFBotController::SetFiring(bool bFire)
{
	if (bFire == bFiring)
	{
		return;
	}
	bFiring = bFire;
	if (ACombatForgeCharacter* Bot = GetBotCharacter())
	{
		// ADS while shooting: tighter spread + CMC ADS flag (reads as aiming, not hip-fire).
		// TP rifle pose is driven by GetBaseAimRotation every tick either way.
		Bot->SetADS(bFire);

		if (UPFWeaponComponent* Weapon = Bot->GetWeapon())
		{
			if (bFire)
			{
				Weapon->StartFire();
				// This press IS the first pull. Charge the re-pull cadence now, or a timer left at
				// <=0 from the previous engagement cycles the trigger again on this same frame and
				// the bot opens with a double shot. (P2-AI2)
				TriggerPullTimer = (Weapon->GetFireMode() == EPFFireMode::Single) ? 0.45f : 0.85f;
			}
			else
			{
				Weapon->StopFire();
			}
		}
	}
}
