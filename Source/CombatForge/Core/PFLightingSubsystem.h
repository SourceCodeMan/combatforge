// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PFLightingSubsystem.generated.h"

/**
 * Spawns the runtime lighting rig (sun + SkyAtmosphere + real-time SkyLight + height fog + volumetric
 * clouds + a graded PostProcess volume) on EVERY machine — host and each remote client.
 *
 * Lighting is purely local/cosmetic and must never replicate; a light actor only contributes if it
 * EXISTS in that machine's world. Spawning it from the (server-only) GameMode left remote clients with
 * an unlit, near-black scene. This world subsystem runs its OnWorldBeginPlay locally on every client
 * (same pattern as UPFSplatSubsystem), and skips dedicated servers which never render. The level itself
 * stays empty (contract T17) — the rig is still zero authored assets, just spawned client-side.
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
