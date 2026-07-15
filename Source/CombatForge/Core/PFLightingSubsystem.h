// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PFLightingSubsystem.generated.h"

/**
 * Art-directed runtime lighting for the 64×40 m arena (CQB warehouse mood).
 *
 * Spawns on EVERY rendering machine (host + remote clients) — lights never replicate.
 * Skips dedicated servers. Zero authored assets (T17): all actors are engine natives.
 *
 * Rig layers (motivated industrial interior, not "asset viewer" flat light):
 *   1. Atmosphere + cool ambient sky (low fill → real contrast)
 *   2. Hard warm KEY directional (raking sun / clerestory)
 *   3. Soft cool FILL directional opposite (no shadows — bounce stand-in)
 *   4. High-bay practicals (warm ceiling pools — readable mid-field)
 *   5. Rim / edge spots (north/south wall wash — silhouette separation)
 *   6. Spawn-end accents (subtle team-side warmth/cool at the long axis ends)
 *   7. Warm-up pen practicals
 *   8. Height fog + unbound filmic post-process (locked exposure)
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
