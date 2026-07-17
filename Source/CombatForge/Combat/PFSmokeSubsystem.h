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
 * Registry of active smoke clouds so SMOKE ACTUALLY CONCEALS for bots: IsSegmentSmoked breaks bot LOS
 * (treated like a sight-line wall). This is AI-only — paintballs / traces never consult this registry and
 * smoke mesh/collider is NoCollision, so bullets always fly through. Server-side (bots live on the host);
 * clients just see the cosmetic cloud.
 */
UCLASS()
class COMBATFORGE_API UPFSmokeSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void RegisterSmoke(const FVector& Center, float Radius, float DurationSec);

	/** True if a live smoke sphere intersects segment A→B (or contains an endpoint). */
	bool IsSegmentSmoked(const FVector& A, const FVector& B) const;

private:
	mutable TArray<FPFSmokeSphere> Smokes;   // lazily pruned in IsSegmentSmoked
};
