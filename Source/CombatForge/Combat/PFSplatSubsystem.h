// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/TimerHandle.h"
#include "Core/CombatForgeTypes.h"
#include "PFSplatSubsystem.generated.h"

class AActor;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UDecalComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UNiagaraSystem;
class APFPaintballProjectile;

/**
 * Client-side world splats (B10, 02 §3.4, contract §3.4).
 *
 * Pool of 256 deferred-decal impact marks (airsoft BB scuffs projected onto receiving surfaces),
 * tinted through four shared team MIDs: confirmed (full team color) and pending (60% brightness)
 * per team, plus a small mesh dust-puff pool for hit juice. Round-robin recycles the oldest.
 * Marks persist across rounds within a match (T20); ResetPool() fires on BuildPhase entry only —
 * this subsystem self-binds to the GameState phase delegate.
 *
 * Pending/confirmed reconcile: the owning client's cosmetic projectile leaves a pending
 * splat; a confirmed splat within 75 uu consumes it (04 §5.2). Unconfirmed pendings expire
 * after 0.6 s with a 0.2 s fade-out (SetFadeOut).
 *
 * Also hosts the per-client cosmetic-projectile pool of 64 (intra to pkg-weapons; 04 §5.3).
 * Does nothing on dedicated/server-only worlds. Never replicates.
 */
UCLASS()
class COMBATFORGE_API UPFSplatSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static constexpr int32 PoolSize = 256;           // B10; round-robin recycle oldest
	static constexpr float ReconcileRadiusUU = 75.f; // 04 §5.2

	UPFSplatSubsystem();

	void SpawnConfirmedSplat(const FVector& Loc, const FVector& Normal, uint8 Team);
		// consumes a matching pending splat within 75 uu if one exists (owning client)
	void SpawnPendingSplat(uint32 ShotIndex, const FVector& Loc, const FVector& Normal, uint8 Team);
		// 60% brightness variant; auto-fades 0.2 s if never confirmed within 0.6 s
	void ResetPool();  // clients call on BuildPhase entry (T20); splats persist across rounds

	// ---- Intra (pkg-weapons): cosmetic projectile pool, 64 per client (04 §5.3) ----
	static constexpr int32 CosmeticProjectilePoolSize = 64;
	APFPaintballProjectile* AcquireCosmeticProjectile();   // nullptr on non-rendering worlds

	// ---- UWorldSubsystem ----
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

private:
	struct FSplatMeta
	{
		bool   bActive = false;
		bool   bPending = false;
		uint32 ShotIndex = 0;
		double SpawnTime = 0.0;
		FVector Loc = FVector::ZeroVector;
		uint8  Team = 0;
	};

	struct FPuffMeta
	{
		bool   bActive = false;
		double HideAt = 0.0;
		double SpawnTime = 0.0;
		float  StartScale = 1.f;
		float  EndScale = 1.f;
		uint8  TeamIdx = 0;
	};

	bool  IsRenderingWorld() const;                  // false on dedicated servers
	void  EnsureInfrastructure();                    // holder actor + MIDs (lazy)
	int32 TakeNextSlot();                            // round-robin
	UDecalComponent* GetOrCreateSplatComp(int32 Index);
	void  PlaceSplat(int32 Index, const FVector& Loc, const FVector& Normal, uint8 Team,
	                 bool bPending, uint32 ShotIndex);
	void  SpawnImpactPuff(const FVector& Loc, const FVector& Normal, uint8 Team);
	bool  TrySpawnNiagaraImpact(const FVector& Loc, const FVector& Normal, uint8 Team);
	UStaticMeshComponent* GetOrCreatePuffComp(int32 Index);
	void  TickPendingExpiry();
	void  HandlePhaseChanged(EPFMatchPhase NewPhase);
	void  TryBindGameState();

	UPROPERTY() TObjectPtr<UMaterialInterface> BaseMaterial;
	UPROPERTY() TObjectPtr<UMaterialInterface> DustMaterial;
	UPROPERTY() TObjectPtr<UStaticMesh>        PuffMesh;
	UPROPERTY() TObjectPtr<UNiagaraSystem>     ImpactFX;
	UPROPERTY() TObjectPtr<AActor>             SplatHolder;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> ConfirmedMIDs;   // [teamIdx 0/1]
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> PendingMIDs;     // 60% brightness
	UPROPERTY() TArray<TObjectPtr<UDecalComponent>> SplatComps;   // lazily filled to PoolSize
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> PuffComps;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> PuffSlotMIDs;      // per-pool-slot (fade-safe)

	TArray<FSplatMeta> SplatMeta;
	TArray<FPuffMeta>  PuffMeta;
	int32 NextSlot = 0;
	int32 NextPuffSlot = 0;
	bool  bBoundToGameState = false;
	bool  bTriedImpactNiagara = false;

	static constexpr int32 PuffPoolSize = 64;
	static constexpr float PuffLifetimeSec = 0.16f;
	static constexpr int32 PuffsPerImpact = 3;   // multi-wisp mesh burst when Niagara missing

	UPROPERTY() TArray<TObjectPtr<APFPaintballProjectile>> CosmeticPool;
	int32 NextCosmeticSlot = 0;

	FTimerHandle PendingExpiryTimer;
	FTimerHandle GameStateBindTimer;
};
