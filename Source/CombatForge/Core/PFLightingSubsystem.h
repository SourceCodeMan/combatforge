// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PFLightingSubsystem.generated.h"

/**
 * Spawns the runtime lighting rig (sun + SkyAtmosphere + real-time SkyLight + height fog +
 * volumetric clouds + a graded PostProcess volume) on EVERY machine — host and each remote client.
 *
 * Lighting is purely local/cosmetic and must never replicate. Spawns on OnWorldBeginPlay for
 * game/PIE worlds only (not the front-end if it never loads a game world subsystem the same way —
 * match entry is when this runs; menu can look fine while match is broken if this rig overexposes).
 *
 * Known-good path: single key directional, modest sky, locked histogram exposure min=max=1.
 * Do not use AEM_Manual for packaged clients (caused pure-white in-match frames).
 */
class APostProcessVolume;
struct FPostProcessSettings;

UCLASS()
class COMBATFORGE_API UPFLightingSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	/** Options-menu sliders land here: offsets the rig's BASE exposure/contrast (never replaces
	 *  them). The base values stay frozen in SpawnLightingRig — this is the only sanctioned
	 *  user-facing brightness path (see commit 76e217d). */
	void SetUserGrade(float BrightnessEV, float ContrastScale);

private:
	void SpawnLightingRig(UWorld& World);
	/** Single owner of the grade math — used at rig spawn AND at runtime so the paths never drift. */
	static void ApplyUserGradeToSettings(FPostProcessSettings& PP, float BrightnessEV, float ContrastScale);

	TWeakObjectPtr<APostProcessVolume> GradedPPV;   // spawned rig PPV (per-world)
};
