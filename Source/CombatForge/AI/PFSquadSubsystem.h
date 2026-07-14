// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PFSquadSubsystem.generated.h"

/**
 * One team-shared enemy sighting: where a teammate last saw a given enemy, and when. Plain struct (server-only,
 * transient) — TWeakObjectPtr is GC-safe and auto-nulls when the enemy pawn dies.
 */
struct FPFSquadSighting
{
	TWeakObjectPtr<AActor> Enemy;
	FVector Pos = FVector::ZeroVector;
	float   Time = -1000.f;
};

/**
 * Squad coordination blackboard (Phase 4). A single per-world, server-side board of enemy sightings, keyed by
 * team. Bots report the enemies they can see; a bot with no target of its own asks for the team's nearest fresh
 * sighting and moves to investigate it — so the squad converges on a threat one teammate spotted instead of each
 * bot fighting blind. Team modes only (Free-for-All bots are solo, not a squad).
 */
UCLASS()
class COMBATFORGE_API UPFSquadSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** A bot currently perceiving Enemy shares it with its team (deduped by enemy — latest pos/time wins). */
	void ReportEnemy(uint8 Team, AActor* Enemy, const FVector& Pos, float Now);

	/**
	 * Nearest still-fresh team sighting to FromPos — a lead for a bot with no target of its own. Skips a sighting
	 * of SelfExclude (don't chase your own report) and expired ones. Returns false if the team has no live lead.
	 */
	bool GetSharedLead(uint8 Team, const FVector& FromPos, float Now, float MaxAge,
	                   const AActor* SelfExclude, FVector& OutPos) const;

private:
	static constexpr int32 NumTeams = 2;   // 0 / 1; FFA + unassigned never use the board
	TArray<FPFSquadSighting> Sightings[NumTeams];
};
