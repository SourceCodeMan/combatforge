// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PFSmokeSubsystem.generated.h"

/** One active smoke cloud, approximated as a bounding sphere. Self-expires by timestamp (no unregistration). */
struct FPFSmokeSphere
{
	FVector Center = FVector::ZeroVector;
	float Radius = 300.f;
	float StartTime = 0.f;   // occlusion begins slightly after detonation (matches the visual billow-in)
	float EndTime = 0.f;     // and ends as the cloud starts dissolving
};

/**
 * Registry of active smoke clouds so SMOKE ACTUALLY CONCEALS: bot line-of-sight consults IsSegmentSmoked and
 * treats a smoked segment like world geometry (sight broken). Server-side users only (bots live on the host);
 * clients simply see the cosmetic cloud.
 */
UCLASS()
class PAINTFORGE_API UPFSmokeSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void RegisterSmoke(const FVector& Center, float Radius, float DurationSec);

	/** True if a live smoke sphere intersects segment A→B (or contains an endpoint). */
	bool IsSegmentSmoked(const FVector& A, const FVector& B) const;

private:
	mutable TArray<FPFSmokeSphere> Smokes;   // lazily pruned in IsSegmentSmoked
};
