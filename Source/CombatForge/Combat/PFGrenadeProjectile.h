// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"
#include "Core/CombatForgeTypes.h"
#include "PFGrenadeProjectile.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UProjectileMovementComponent;
class UPFWeaponComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/**
 * Thrown grenade (frag or smoke). Unlike the paintball this is GENUINELY replicated + movement-replicated
 * so the lobbed arc is seen by everyone. The server arms a fuse, then detonates:
 *
 *   Frag  = a radial burst of authoritative APFPaintballProjectile BBs — reuses the existing
 *           hit -> UPFHealthComponent::ApplyPaintHit -> elimination + splat pipeline, so it is a literal
 *           non-lethal "burst of BBs". Team-tagged, so teammates are immune (B12). Cover blocks it because
 *           each BB is a real swept projectile (no through-wall radial).
 *   Smoke = a translucent concealment cloud. Mesh puffs + post-pop root are NoCollision so paintballs
 *           pass straight through; bot LOS alone treats the cloud as a sight-breaker (not a bullet shield).
 *
 * Detonation cosmetics/audio replicate via bDetonated + OnRep (robust for the smoke's whole lifetime and
 * for late-relevant clients); the listen host runs them directly in ServerDetonate. Non-host clients rebuild
 * the frag tracer burst from the replicated seed so tracers line up with the authoritative BBs.
 */
UCLASS()
class COMBATFORGE_API APFGrenadeProjectile : public AActor
{
	GENERATED_BODY()

public:
	APFGrenadeProjectile();

	/** Authority only: launch along AimDir, remember team/type/thrower, arm the fuse. */
	void ServerInit(const FVector& AimDir, uint8 Team, EPFGrenadeType Type, UPFWeaponComponent* Thrower);

public:
	virtual void Tick(float DeltaSeconds) override;   // animates the smoke cloud (billow-in + staggered dissolve)

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_Detonated();
	UFUNCTION() void OnRep_Kind();

	void ServerDetonate();
	void OnDestroyTimer();
	void HandleDetonateVisualsLocal();
	void ApplyBallSkin();                                         // tint the thrown ball by KindRep (never the default slot)
	void SpawnFragBurst(const FVector& At, uint32 Seed);          // server: authoritative BBs (damage)
	void SpawnCosmeticFragBurst(const FVector& At, uint32 Seed);  // non-host clients: cosmetic tracers
	void StartSmokeVisual(const FVector& At);
	void PlayDetonAudio(const FVector& At, bool bFrag);

	UPROPERTY() TObjectPtr<USphereComponent>             Collision;
	UPROPERTY() TObjectPtr<UStaticMeshComponent>         Mesh;
	UPROPERTY() TObjectPtr<UProjectileMovementComponent> Movement;
	// Ball skin: MID from M_PF_ArenaMetal (preferred) or the ctor-hard-ref BasicShapeMaterial fallback,
	// tinted by KindRep so both grenade types read instead of riding the checkerboard default slot.
	UPROPERTY() TObjectPtr<UMaterialInterface>           BallBaseMaterial;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic>     BallMID;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> SmokePuffs;
	// Per-puff smoke animation state (billow-in over SmokeAppearDur, then each fades on its own window).
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> SmokeMIDs;
	TArray<FVector> PuffTargetScale;
	TArray<float>   PuffMaxDensity;
	TArray<float>   PuffDissolveWindow;   // seconds-before-end at which this puff starts fading
	TArray<float>   PuffYawRateDeg;       // per-puff lazy swirl, deg/s (signed)
	TArray<float>   PuffDriftRate;        // per-puff upward drift, uu/s (scaled by the billow-in)
	TArray<float>   PuffWobblePhase;      // per-puff sin phase for the ±8% scale wobble
	float SmokeStartTime = -1.f;
	/** CoD-style instant pop (~120 ms) — GZW's multi-second billow is too slow for paintball tempo. */
	float SmokeAppearDur = 0.12f;
	bool  bSmokeVolumetric = false;       // true when M_PF_SmokeVolume is in use (drives the Density param)

	UPROPERTY(ReplicatedUsing=OnRep_Detonated) bool bDetonated = false;
	UPROPERTY(ReplicatedUsing=OnRep_Kind) uint8  KindRep = 0;      // EPFGrenadeType, so clients build the right FX + skin
	UPROPERTY(Replicated) uint32 BurstSeed = 0;    // shared frag spread seed (server + client tracers agree)
	UPROPERTY(Replicated) FVector_NetQuantize100 DetonatePoint = FVector::ZeroVector;

	// ---- Config ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") float ThrowSpeed    = 1650.f;   // original toss speed (throw was right; only the spawn was wrong)
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") float FuseSeconds   = 3.6f;   // +2s per playtest 2026-08-07 (was 1.6)
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") int32 FragBBCount   = 90;       // 3x pellets
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") float SmokeDuration = 8.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") float SmokeRadius   = 300.f;

	TWeakObjectPtr<UPFWeaponComponent> ThrowerWeak;   // server-only; clients fall back to null (cosmetic ballistics)
	// Replicated: remote clients rebuild the cosmetic frag burst + splats from this — as a plain member it
	// was always team 0 on clients, so enemy frag tracers/splats wore the wrong team color (issue #11 CB1).
	UPROPERTY(Replicated) uint8 TeamId = 0;
	EPFGrenadeType Kind = EPFGrenadeType::Frag;
	FTimerHandle FuseTimer;
	FTimerHandle DestroyTimer;
};
