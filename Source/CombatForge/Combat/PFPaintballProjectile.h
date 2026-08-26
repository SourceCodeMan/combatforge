// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/HitResult.h"
#include "PFPaintballProjectile.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProjectileMovementComponent;
class USphereComponent;
class UStaticMeshComponent;
class UPFWeaponComponent;

/**
 * One paintball class, two modes (B3, T10, contract §3.4). NEVER replicated.
 *
 *  - Authoritative (server only): real blocking sweep vs Pawn/World on PF_ECC_Paintball,
 *    shooter ignored (no self-splat). Pawn hit → FPFPaintHitInfo → victim UPFHealthComponent
 *    + ClientHitConfirm + MulticastImpactSplat (teammate → splat only, B12). World hit →
 *    MulticastImpactSplat. Destroyed on hit or 2.0 s.
 *  - Cosmetic (clients): no gameplay collision; per-tick visual sweep for the predicted
 *    impact; owning client registers a PENDING splat (UPFSplatSubsystem::SpawnPendingSplat),
 *    then parks back in the per-client pool of 64 (lives on UPFSplatSubsystem — intra).
 *
 * Ballistics: 10,000 uu/s, gravity ×0.35, life 2.0 s, r≈2.33 uu sphere (~1/3 original BB),
 * no bounce/penetration, no inherited shooter velocity (04 §2.2). Visual: elongated engine
 * sphere tracer, emissive-boosted team MID for airsoft BB trail readability.
 */
UCLASS()
class COMBATFORGE_API APFPaintballProjectile : public AActor
{
	GENERATED_BODY()

public:
	APFPaintballProjectile();  // bReplicates=false; sphere on PF_ECC_Paintball;
	                           // UProjectileMovementComponent (10000, grav 0.35, no bounce);
	                           // engine-sphere visual, emissive team MID

	// Single init point; caller applies spread to Dir BEFORE calling (shared stream — B3).
	// bIgnoreShooter=false for breach bombs so the planter can be tagged by their own charge.
	// bPointBlankRescue=false for the frag cloud — thrower stays self-immune, no Eye→Origin sweep.
	void InitProjectile(const FVector& Origin, const FVector& SpreadedDir, uint8 Team,
	                    bool bAuthoritative, UPFWeaponComponent* SourceWeapon, uint32 ShotIndex,
	                    bool bIgnoreShooter = true, bool bPointBlankRescue = true);

	/** P2-CB1/CB2: utility bursts (bomb breach spray, frag cloud) do FIXED per-ball damage —
	 *  never the planter's per-gun HitValue rank lever (a sniper primary made every burst ball
	 *  multi-lethal). 0 = live-fire ball: damage comes from SourceWeapon->HitValue as always.
	 *  Set before InitProjectile — the point-blank rescue can resolve a hit inside Init. */
	uint8 UtilityDamageOverride = 0;

	virtual void Tick(float DeltaSeconds) override;

	// ---- Intra pool interface (UPFSplatSubsystem owns the cosmetic pool) ----
	void Deactivate();               // park: hide, stop movement, disable tick/collision
	bool IsInFlight() const { return bInFlight; }

protected:
	UFUNCTION() void HandleProjectileStop(const FHitResult& ImpactResult);

private:
	void ResolveAuthoritativeImpact(const FHitResult& Hit);
	void HandleCosmeticImpact(const FHitResult& Hit);
	static uint8 VictimTeamOf(const AActor* HitActor);   // 255 = teamless (dummy/world)

	UPROPERTY() TObjectPtr<USphereComponent>              CollisionComp;
	UPROPERTY() TObjectPtr<UStaticMeshComponent>          BallMesh;
	UPROPERTY() TObjectPtr<UProjectileMovementComponent>  Movement;
	UPROPERTY() TObjectPtr<UMaterialInterface>            BaseMaterial;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic>      BallMID;

	TWeakObjectPtr<UPFWeaponComponent> SourceWeaponWeak;
	TWeakObjectPtr<AActor>             ShooterActorWeak;

	uint8   TeamId = 0;
	uint32  ShotIndexStored = 0;
	bool    bAuthoritativeMode = false;
	bool    bInFlight = false;
	float   LifeElapsed = 0.f;
	float   LifetimeSec = 2.f;
	float   RadiusUU = 2.33f;
	FVector PrevLocation = FVector::ZeroVector;
};
