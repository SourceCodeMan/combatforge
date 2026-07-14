// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "AI/PFSquadSubsystem.h"

namespace
{
	// Sightings older than this are dropped on the next report (belt for the MaxAge query filter).
	constexpr float GStaleAfterSec = 12.f;
}

void UPFSquadSubsystem::ReportEnemy(uint8 Team, AActor* Enemy, const FVector& Pos, float Now)
{
	if (Team >= NumTeams || Enemy == nullptr)
	{
		return;
	}
	TArray<FPFSquadSighting>& List = Sightings[Team];

	// Update in place if we already track this enemy; otherwise append. Prune dead/stale entries as we scan.
	bool bUpdated = false;
	for (int32 i = List.Num() - 1; i >= 0; --i)
	{
		FPFSquadSighting& S = List[i];
		if (!S.Enemy.IsValid() || (Now - S.Time) > GStaleAfterSec)
		{
			List.RemoveAtSwap(i);
			continue;
		}
		if (S.Enemy.Get() == Enemy)
		{
			S.Pos = Pos;
			S.Time = Now;
			bUpdated = true;
		}
	}
	if (!bUpdated)
	{
		FPFSquadSighting New;
		New.Enemy = Enemy;
		New.Pos = Pos;
		New.Time = Now;
		List.Add(New);
	}
}

bool UPFSquadSubsystem::GetSharedLead(uint8 Team, const FVector& FromPos, float Now, float MaxAge,
                                      const AActor* SelfExclude, FVector& OutPos) const
{
	if (Team >= NumTeams)
	{
		return false;
	}
	const TArray<FPFSquadSighting>& List = Sightings[Team];

	float BestSq = TNumericLimits<float>::Max();
	bool bFound = false;
	for (const FPFSquadSighting& S : List)
	{
		if (!S.Enemy.IsValid() || (Now - S.Time) > MaxAge || S.Enemy.Get() == SelfExclude)
		{
			continue;
		}
		const float Sq = FVector::DistSquared(FromPos, S.Pos);
		if (Sq < BestSq)
		{
			BestSq = Sq;
			OutPos = S.Pos;
			bFound = true;
		}
	}
	return bFound;
}
