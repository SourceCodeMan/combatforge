// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"
#include "Core/PaintForgeTypes.h"
#include "PFGrenadeProjectile.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UProjectileMovementComponent;
class UPFWeaponComponent;

/**
 * Thrown grenade (frag or smoke). Unlike the paintball this is GENUINELY replicated + movement-replicated
 * so the lobbed arc is seen by everyone. The server arms a fuse, then detonates:
 *
 *   Frag  = a radial burst of authoritative APFPaintballProjectile BBs — reuses the existing
 *           hit -> UPFHealthComponent::ApplyPaintHit -> elimination + splat pipeline, so it is a literal
 *           non-lethal "burst of BBs". Team-tagged, so teammates are immune (B12). Cover blocks it because
 *           each BB is a real swept projectile (no through-wall radial).
 *   Smoke = a translucent concealment cloud (cosmetic; no gameplay collision, does not block AI in v1).
 *
 * Detonation cosmetics/audio replicate via bDetonated + OnRep (robust for the smoke's whole lifetime and
 * for late-relevant clients); the listen host runs them directly in ServerDetonate. Non-host clients rebuild
 * the frag tracer burst from the replicated seed so tracers line up with the authoritative BBs.
 */
UCLASS()
class PAINTFORGE_API APFGrenadeProjectile : public AActor
{
	GENERATED_BODY()

public:
	APFGrenadeProjectile();

	/** Authority only: launch along AimDir, remember team/type/thrower, arm the fuse. */
	void ServerInit(const FVector& AimDir, uint8 Team, EPFGrenadeType Type, UPFWeaponComponent* Thrower);

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_Detonated();

	void ServerDetonate();
	void OnDestroyTimer();
	void HandleDetonateVisualsLocal();
	void SpawnFragBurst(const FVector& At, uint32 Seed);          // server: authoritative BBs (damage)
	void SpawnCosmeticFragBurst(const FVector& At, uint32 Seed);  // non-host clients: cosmetic tracers
	void StartSmokeVisual(const FVector& At);
	void PlayDetonAudio(const FVector& At, bool bFrag);

	UPROPERTY() TObjectPtr<USphereComponent>             Collision;
	UPROPERTY() TObjectPtr<UStaticMeshComponent>         Mesh;
	UPROPERTY() TObjectPtr<UProjectileMovementComponent> Movement;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> SmokePuffs;

	UPROPERTY(ReplicatedUsing=OnRep_Detonated) bool bDetonated = false;
	UPROPERTY(Replicated) uint8  KindRep = 0;      // EPFGrenadeType, so clients build the right FX
	UPROPERTY(Replicated) uint32 BurstSeed = 0;    // shared frag spread seed (server + client tracers agree)
	UPROPERTY(Replicated) FVector_NetQuantize100 DetonatePoint = FVector::ZeroVector;

	// ---- Config ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") float ThrowSpeed    = 3400.f;   // fast, flat throw (was a slow lob)
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") float FuseSeconds   = 1.4f;     // trimmed so the faster nade doesn't sail too far
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") int32 FragBBCount   = 30;
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") float SmokeDuration = 8.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") float SmokeRadius   = 300.f;

	TWeakObjectPtr<UPFWeaponComponent> ThrowerWeak;   // server-only; clients fall back to null (cosmetic ballistics)
	uint8 TeamId = 0;
	EPFGrenadeType Kind = EPFGrenadeType::Frag;
	FTimerHandle FuseTimer;
	FTimerHandle DestroyTimer;
};
