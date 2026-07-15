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
UCLASS()
class COMBATFORGE_API UPFLightingSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

private:
	void SpawnLightingRig(UWorld& World);
};
