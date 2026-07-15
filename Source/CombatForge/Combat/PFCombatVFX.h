// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/TimerHandle.h"
#include "PFCombatVFX.generated.h"

class UNiagaraSystem;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMesh;
class UStaticMeshComponent;
class UPointLightComponent;

/**
 * Client-side combat VFX (muzzle CO₂ / flash + soft-ref Niagara).
 *
 * Soft-loads /Game/FX Niagara systems when present (Fab pack or Lyra migrate); otherwise
 * spawns pooled mesh flash + CO₂ wisps from M_PF_Flash / M_PF_MuzzleSmoke so fire still
 * reads "juicy" without marketplace content.
 *
 * Non-replicated — call only on rendering clients (same sites as PlayMuzzle).
 * Muzzle is neutral white/warm (not team-tinted). Impacts live on UPFSplatSubsystem.
 */
UCLASS()
class COMBATFORGE_API UPFCombatVFX : public UActorComponent
{
	GENERATED_BODY()

public:
	UPFCombatVFX();

	/** One-shot muzzle report at world location, aimed along ShotDir. */
	void PlayMuzzleFX(const FVector& MuzzleLoc, const FVector& ShotDir, bool bFirstPerson);

	/** Sample owner character muzzle + aim. */
	void PlayMuzzleFXForOwner(bool bCosmeticFirstPerson);

private:
	struct FMuzzlePartMeta
	{
		bool bActive = false;
		double HideAt = 0.0;
		float StartScale = 1.f;
		float EndScale = 1.f;
		uint8 Kind = 0;   // 0 = flash core, 1 = smoke wisp
	};

	bool CanPlay() const;
	void EnsureAssets();
	void EnsureMeshPool();
	void TickMuzzleParts();
	bool TrySpawnNiagaraMuzzle(const FVector& Loc, const FVector& Dir, bool bFirstPerson);
	void SpawnMeshMuzzle(const FVector& Loc, const FVector& Dir, bool bFirstPerson);

	// Soft Niagara (optional content under /Game/FX).
	UPROPERTY(Transient) TObjectPtr<UNiagaraSystem> NS_MuzzleFlash;
	UPROPERTY(Transient) TObjectPtr<UNiagaraSystem> NS_MuzzleSmoke;

	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> FlashMat;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> SmokeMat;
	UPROPERTY(Transient) TObjectPtr<UStaticMesh> SphereMesh;

	// World-space pool (not parented to the gun — free-floating wisps).
	UPROPERTY(Transient) TObjectPtr<AActor> VfxHolder;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> PoolMeshes;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> PoolFlashMIDs;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> PoolSmokeMIDs;
	UPROPERTY(Transient) TObjectPtr<UPointLightComponent> MuzzleLight;

	TArray<FMuzzlePartMeta> PoolMeta;
	int32 NextPoolSlot = 0;
	bool bAssetsReady = false;
	bool bTriedNiagara = false;

	FTimerHandle TickTimer;

	static constexpr int32 PoolSize = 24;
	static constexpr float FlashLifeSec = 0.045f;
	static constexpr float SmokeLifeSec = 0.22f;
};
