// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFSmokeSubsystem.h"

#include "Engine/World.h"

void UPFSmokeSubsystem::RegisterSmoke(const FVector& Center, float Radius, float DurationSec)
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	const float Now = static_cast<float>(World->GetTimeSeconds());
	FPFSmokeSphere S;
	S.Center = Center;
	S.Radius = Radius;
	S.StartTime = Now + 0.3f;                                 // billow-in (visual SmokeAppearDur is 0.6s)
	S.EndTime = Now + FMath::Max(1.f, DurationSec - 1.5f);    // stop occluding as the cloud dissolves
	Smokes.Add(S);
}

bool UPFSmokeSubsystem::IsSegmentSmoked(const FVector& A, const FVector& B) const
{
	if (Smokes.Num() == 0)
	{
		return false;
	}
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}
	const float Now = static_cast<float>(World->GetTimeSeconds());
	for (int32 i = Smokes.Num() - 1; i >= 0; --i)
	{
		const FPFSmokeSphere& S = Smokes[i];
		if (Now > S.EndTime)
		{
			Smokes.RemoveAtSwap(i);   // self-expired
			continue;
		}
		if (Now < S.StartTime)
		{
			continue;   // still billowing in
		}
		if (FMath::PointDistToSegment(S.Center, A, B) <= S.Radius)
		{
			return true;   // segment passes through (or ends inside) the cloud
		}
	}
	return false;
}
