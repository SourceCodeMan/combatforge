// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PFWarehouseStreamSubsystem.generated.h"

class ULevelStreamingDynamic;

/**
 * Streams the Megascans Industrial_Warehouse map as a visual environment on every machine
 * (host + clients). Gameplay stays on APFArenaShell (spawns, build grid, midline, pen).
 *
 * Package path is soft — if Scene_Warehouse isn't on disk, we no-op and the cube shell remains.
 */
UCLASS()
class COMBATFORGE_API UPFWarehouseStreamSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	/** True after a successful LoadLevelInstance this session. */
	bool IsWarehouseStreamed() const { return bStreamSucceeded; }

private:
	void TryStreamWarehouse(UWorld& World);
	void ApplyShellBackdrop(UWorld& World);

	UPROPERTY() TObjectPtr<ULevelStreamingDynamic> StreamedWarehouse;
	bool bStreamSucceeded = false;
	FTimerHandle BackdropRetryHandle;
};
