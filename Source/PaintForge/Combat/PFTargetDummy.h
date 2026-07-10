// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/TimerHandle.h"
#include "Core/PaintForgeTypes.h"
#include "PFTargetDummy.generated.h"

class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UPFHealthComponent;

/**
 * Warm-up pen target (T29, contract §3.4).
 *
 * Engine-cylinder body (r≈40, h≈180) blocking PF_ECC_Paintball; owns a UPFHealthComponent
 * with DefaultRoundHP=1. Binds its own health OnEliminatedEvent → hide 1.0 s →
 * ResetForRound(1) (self-resetting). Never routes to the GameMode; never grants score
 * (the GameMode only subscribes to player pawns' health components).
 *
 * GameMode spawns one per connected player at APFArenaShell warm-up dummy slots (T29).
 */
UCLASS()
class PAINTFORGE_API APFTargetDummy : public AActor
{
	GENERATED_BODY()

public:
	APFTargetDummy();  // bReplicates=true; cylinder body; UPFHealthComponent(DefaultRoundHP=1)

	UPFHealthComponent* GetHealth() const { return Health; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void HandleEliminated(UPFHealthComponent* HealthComp, const FPFPaintHitInfo& FinalHit);
	void HandleRespawn();

	UPROPERTY(VisibleAnywhere, Category="PF|Dummy") TObjectPtr<UStaticMeshComponent> BodyMesh;
	UPROPERTY(VisibleAnywhere, Category="PF|Dummy") TObjectPtr<UPFHealthComponent>   Health;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> BodyMID;

	FTimerHandle RespawnTimer;
};
