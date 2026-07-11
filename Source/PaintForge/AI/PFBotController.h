// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "PFBotController.generated.h"

class APaintForgeCharacter;

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
UCLASS()
class PAINTFORGE_API APFBotController : public AAIController
{
	GENERATED_BODY()

public:
	APFBotController();

	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

protected:
	// ---- Brain tunables ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float EngageRangeUU = 4500.f;      // max distance to open fire
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float PreferredRangeUU = 1400.f;   // stand-off band centre
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float MinRangeUU = 700.f;          // back up if closer
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float TargetRefreshInterval = 0.4f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float AimErrorDeg = 3.5f;          // beatable, not laser-accurate
	UPROPERTY(EditDefaultsOnly, Category="PF|Bot") float StrafeSwitchInterval = 1.8f;

private:
	APaintForgeCharacter* GetBotCharacter() const;
	APaintForgeCharacter* AcquireNearestEnemy() const;
	bool HasLineOfSight(const APaintForgeCharacter* Target) const;
	void SetFiring(bool bFire);

	TWeakObjectPtr<APaintForgeCharacter> CurrentTarget;
	float TargetRefreshTimer = 0.f;
	float StrafeTimer = 0.f;
	float StrafeSign = 1.f;
	float AimJitterYaw = 0.f;
	float AimJitterPitch = 0.f;
	bool  bFiring = false;
};
