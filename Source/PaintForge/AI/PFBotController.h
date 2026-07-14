// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionTypes.h"   // FAIStimulus (perception callback param)
#include "PFBotController.generated.h"

class APaintForgeCharacter;
class APFControlPointActor;
class APFFlagActor;
class UAIPerceptionComponent;
class UAISenseConfig_Sight;
class UAISenseConfig_Hearing;

/**
 * Server-only roster-filling bot (contract addendum: bots fill teams to the selected format).
 *
 * A bot is an AAIController that possesses a normal APaintForgeCharacter on the listen host, so
 * every combat seam it uses is the SAME authoritative path a host player takes: the fire pipeline
 * (StartFire → ServerFire → authoritative projectile), damage, and elimination all work unchanged.
 * Two things make that hold: bWantsPlayerState=true (so the bot lands in GameState->PlayerArray
 * with an APaintForgePlayerState carrying its TeamId — the deterministic spread seed — and gets
 * counted for alive/scoring/victory), and a per-tick control rotation aimed at the target (the
 * weapon reads GetControlRotation, and the 4° server dir-gate then agrees).
 *
 * The brain is a compact C++ tick (no navmesh/behavior-tree assets — fits the zero-editor-assets
 * ethos): acquire the nearest visible enemy, face it, hold a stand-off band with simple strafing,
 * and fire on line-of-sight. Deliberately imperfect (per-acquisition aim error) so bots are beatable.
 */

/** Bot skill preset — scales aim error, reaction time and engage range. Rookie is the kid-test default. */
UENUM()
enum class EPFBotSkill : uint8
{
	Rookie,        // kids: sloppy aim, slow to notice, shorter engage range
	Regular,       // the original beatable-but-competent brain
	Sharpshooter   // tighter aim, faster reaction — adult scrims
};

UCLASS()
class PAINTFORGE_API APFBotController : public AAIController
{
	GENERATED_BODY()

public:
	APFBotController();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result) override;

	// Friend/foe for AI Perception (the #1 perception gotcha). AAIController implements IGenericTeamAgentInterface;
	// we override the attitude to use the GAME's team rules directly (PlayerState TeamId + FFA), so the sight/
	// hearing affiliation filter works without every player/bot controller needing a matching FGenericTeamId.
	virtual ETeamAttitude::Type GetTeamAttitudeTowards(const AActor& Other) const override;

protected:
	// ---- Brain tunables ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float EngageRangeUU = 4500.f;      // max distance to open fire
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float PreferredRangeUU = 1400.f;   // stand-off band centre
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float MinRangeUU = 700.f;          // back up if closer
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float TargetRefreshInterval = 0.4f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float AimErrorDeg = 3.5f;          // beatable, not laser-accurate (ApplySkill overrides)
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float StrafeSwitchInterval = 1.8f;

	// Kid-test difficulty: Skill picks the numbers in ApplySkill(); ReactionDelay is the "notice" gap
	// before a freshly-acquired target may be fired on. Default Rookie = easy bots for the kids' session.
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") EPFBotSkill Skill = EPFBotSkill::Rookie;
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float ReactionDelay = 0.6f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float AimTurnRate = 2.5f;      // control-rotation ease speed (low = laggy aim, misses strafers)
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float AimJitterInterval = 0.6f;// how often the random aim error is re-rolled

	// Close-quarters engagement: a bot must reliably turn on + hit an enemy in its face. Inside PointBlankUU an
	// enemy is force-targeted (overrides a sticky far target). Inside CloseAimRangeUU the aim tracks faster and
	// tightens (scaled by proximity) so a point-blank foe circling the bot doesn't just walk around the flailing,
	// wide-cone Rookie aim. Distance play is unchanged (still laggy + wide = beatable).
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float PointBlankUU = 1200.f;      // "in your face" — force-target + snap reaction
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float CloseAimRangeUU = 1800.f;   // under this, aim tracking + accuracy ramp up toward contact
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float CloseAimTurnMult = 4.0f;    // turn-rate multiplier at contact (tracks a circling target)
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float CloseAimErrorMult = 0.30f;  // aim-error multiplier at contact (point-blank shots connect)

	// ---- Perception (sight cone + hearing). Sight makes bots flankable at range; a short-range proximity sense
	//      keeps close-quarters reliable (so FOV never regresses the point-blank fix); hearing + last-known-
	//      position let a flanked/broken-LOS bot turn toward gunfire and hunt where it last saw you. ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Perception") float SightRadiusUU = 5000.f;     // start seeing a hostile within this range (in the cone)
	UPROPERTY(EditDefaultsOnly, Category="PF|Perception") float SightLoseRadiusUU = 5600.f; // keep seeing until beyond this (must be >= SightRadius)
	UPROPERTY(EditDefaultsOnly, Category="PF|Perception") float SightFOVHalfDeg = 100.f;    // HALF-angle from forward → 200° total cone (wide peripheral)
	UPROPERTY(EditDefaultsOnly, Category="PF|Perception") float SightAutoSeeUU = 1000.f;    // auto-see a hostile this close to where it was last seen
	UPROPERTY(EditDefaultsOnly, Category="PF|Perception") float HearingRangeUU = 4500.f;    // hear gunfire/footsteps within this range
	UPROPERTY(EditDefaultsOnly, Category="PF|Perception") float ProximityAwareUU = 1800.f;  // 360° "sixth sense": a hostile this close (with LOS) is always noticed
	UPROPERTY(EditDefaultsOnly, Category="PF|Perception") float SearchHoldSec = 6.f;        // how long to hunt a last-known-position / investigate a noise before giving up

	// ---- Tactical positioning (EQS-lite): instead of standing in the open, periodically pick the best nearby
	//      FIRING POSITION — line of sight to shoot, near cover, good range, off the enemy's facing (flank),
	//      spread from teammates — and reposition there. Makes bots use the fort + flank as a group. ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Tactics") float RepositionInterval = 1.2f;   // seconds between firing-position re-evaluations
	UPROPERTY(EditDefaultsOnly, Category="PF|Tactics") float ReposSampleNearUU = 350.f;   // inner ring radius of candidate points
	UPROPERTY(EditDefaultsOnly, Category="PF|Tactics") float ReposSampleFarUU = 850.f;    // outer ring radius of candidate points
	UPROPERTY(EditDefaultsOnly, Category="PF|Tactics") float CoverProbeUU = 250.f;        // how far to probe for adjacent cover around a candidate
	UPROPERTY(EditDefaultsOnly, Category="PF|Tactics") float SpreadRadiusUU = 700.f;      // penalize candidate points within this of a teammate (de-clump)

	// ---- Accuracy under pressure (Gray-Zone-style): being shot or hurt degrades aim + drives the bot to cover.
	UPROPERTY(EditDefaultsOnly, Category="PF|Accuracy") float SuppressDurationSec = 1.6f;  // "under fire" window after taking a hit
	UPROPERTY(EditDefaultsOnly, Category="PF|Accuracy") float SuppressErrorMult = 2.2f;    // aim cone widens this much while suppressed
	UPROPERTY(EditDefaultsOnly, Category="PF|Accuracy") float InjuryErrorMaxMult = 1.8f;   // aim cone at near-death vs full health

	// Navmesh pathfinding (replaces the old reactive whisker-steer): the bot picks a tactical GOAL POINT
	// and the RecastNavMesh routes it there, so it walks AROUND the player-built fort instead of into it.
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float ObjectiveHoldRadiusUU = 220.f; // within this of the point/flag = "on it" (hold + strafe)
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float GoalProjectUU = 700.f;  // how far ahead the tactical heading is projected into a move goal
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float MoveAcceptUU = 70.f;    // "arrived" tolerance for a MoveTo request
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float RepathInterval = 0.35f; // min seconds between re-issued moves (re-pathing every tick thrashes PathFollowing)
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float RepathMoveThreshUU = 150.f; // re-path early only when the goal jumped at least this far

private:
	APaintForgeCharacter* GetBotCharacter() const;
	APaintForgeCharacter* AcquireNearestEnemy() const;   // nearest hostile the bot can SEE (perception) or feel (proximity)
	bool IsHostilePlayerState(const class APaintForgePlayerState* OtherPS) const;   // game team rules (mirrors GetTeamAttitudeTowards)
	bool HasLineOfSight(const APaintForgeCharacter* Target) const;

	// Perception callback (must be UFUNCTION — OnTargetPerceptionUpdated is a dynamic delegate). Hearing stimuli
	// become an "investigate this noise" goal; sight is polled directly in the target scan.
	UFUNCTION()
	void OnPerceptionStimulus(AActor* Actor, FAIStimulus Stimulus);
	void SetFiring(bool bFire);
	void ApplySkill();          // map Skill → AimErrorDeg / ReactionDelay / EngageRangeUU (called on possess)
	void MoveToGoal(const FVector& RawGoal, AActor* FallbackActor);   // navmesh MoveTo toward a tactical point (falls back to the enemy if the point is off-mesh)
	FVector ChooseTacticalPosition(const APaintForgeCharacter* Target) const;   // EQS-lite: best nearby firing position
	bool IsTargetEngageable(const APaintForgeCharacter* Target) const; // alive + in range + visible (sticky-target gate)
	bool ComputeObjectiveGoal(FVector& OutGoal);   // Dom/Hardpoint/CTF: where to push (false in fight modes)
	void EnsureObjectivesCached();                 // lazily grab the control-point / flag actors (once per match)

	TWeakObjectPtr<APaintForgeCharacter> CurrentTarget;
	float TargetRefreshTimer = 0.f;
	float FireHoldTimer = 0.f;   // counts down after acquiring a NEW target; fire is blocked until <= 0 (reaction gap)
	float StrafeTimer = 0.f;
	float StrafeSign = 1.f;
	float AimJitterYaw = 0.f;
	float AimJitterPitch = 0.f;
	float AimJitterTimer = 0.f;
	// Navmesh path-following state: throttle re-pathing by time + goal displacement so a fresh MoveTo
	// doesn't abort the last one every tick (which thrashes PathFollowing → jitter).
	FVector LastPathedGoal = FVector::ZeroVector;
	float RepathTimer = 0.f;
	FAIRequestID CurrentMoveId;
	float NavWarnTimer = 0.f;   // throttles the "no navmesh under me" diagnostic so it can't spam the log
	// Objective-mode targets (Domination / Hardpoint / CTF), cached once per match.
	TArray<TWeakObjectPtr<APFControlPointActor>> ControlPointsCache;
	TArray<TWeakObjectPtr<APFFlagActor>> FlagsCache;
	bool  bFiring = false;
	bool  bFireModeAssigned = false;   // one-shot per spawn: pick a fire mode from the weapon's allowed set

	// ---- Perception components (created in ctor) ----
	UPROPERTY() TObjectPtr<UAIPerceptionComponent> AIPerception;
	UPROPERTY() TObjectPtr<UAISenseConfig_Sight> SightConfig;
	UPROPERTY() TObjectPtr<UAISenseConfig_Hearing> HearingConfig;
	FAISenseID HearingSenseID;   // cached in BeginPlay to tag hearing stimuli in the callback

	// ---- Search / investigate memory ----
	// LastSeen* = where we last actually saw the target (hunt here when LOS breaks). Investigate* = a heard-noise
	// location to go check. Both expire after SearchHoldSec so a bot doesn't hunt a ghost forever.
	FVector LastSeenPos = FVector::ZeroVector;
	float   LastSeenTime = -1000.f;
	FVector InvestigatePos = FVector::ZeroVector;
	float   InvestigateTime = -1000.f;

	// ---- Tactical reposition state: the current chosen firing position + its re-evaluation timer. ----
	FVector TacticalGoal = FVector::ZeroVector;
	bool    bHaveTacticalGoal = false;
	float   ReposTimer = 0.f;

	// ---- Suppression / injury state: detect taking a hit (HP drop) to widen aim + break for cover. ----
	uint8 LastKnownHP = 255;         // 255 = uninitialised (first tick this life)
	uint8 RoundMaxHP = 1;            // highest HP seen this life → the injury denominator
	float SuppressedUntil = -1000.f; // world time until which the bot is "under fire"
};
