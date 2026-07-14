// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "PFBotController.generated.h"

class APaintForgeCharacter;
class APFControlPointActor;
class APFFlagActor;

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

	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result) override;

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

	// Navmesh pathfinding (replaces the old reactive whisker-steer): the bot picks a tactical GOAL POINT
	// and the RecastNavMesh routes it there, so it walks AROUND the player-built fort instead of into it.
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float ObjectiveHoldRadiusUU = 220.f; // within this of the point/flag = "on it" (hold + strafe)
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float GoalProjectUU = 700.f;  // how far ahead the tactical heading is projected into a move goal
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float MoveAcceptUU = 70.f;    // "arrived" tolerance for a MoveTo request
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float RepathInterval = 0.35f; // min seconds between re-issued moves (re-pathing every tick thrashes PathFollowing)
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float RepathMoveThreshUU = 150.f; // re-path early only when the goal jumped at least this far

private:
	APaintForgeCharacter* GetBotCharacter() const;
	APaintForgeCharacter* AcquireNearestEnemy() const;
	bool HasLineOfSight(const APaintForgeCharacter* Target) const;
	void SetFiring(bool bFire);
	void ApplySkill();          // map Skill → AimErrorDeg / ReactionDelay / EngageRangeUU (called on possess)
	void MoveToGoal(const FVector& GoalLoc);   // issue a navmesh MoveTo (strafe-facing) toward a tactical point
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
};
