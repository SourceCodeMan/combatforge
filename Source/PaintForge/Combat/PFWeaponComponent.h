// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Math/RandomStream.h"
#include "Core/PaintForgeTypes.h"
#include "PFWeaponComponent.generated.h"

class APaintForgeCharacter;
class APaintForgeGameState;
class APaintForgePlayerState;
class UPFCharacterMovementComponent;

/**
 * The marker (contract §3.4, 04 §2/§3/§5).
 *
 * Fake-and-verify fire pipeline (B3):
 *   Owning client: consume ROF token → spawn Cosmetic APFPaintballProjectile using the
 *   shared spread stream → UPFFireShake → predicted hopper decrement → ServerFire(packet).
 *   Server: token bucket (cap 3, refill 12/s) + origin ≤150 uu + dir ≤4° + phase gate
 *   (GameState->IsFireAllowed, T21) → Authoritative projectile with the SAME stream.
 *   Remote clients: MulticastShotFX → their own cosmetic. Hitmarkers ONLY via ClientHitConfirm.
 *
 * On a listen host the authoritative projectile is the visual: the host spawns no cosmetic
 * for its own shots and skips MulticastShotFX locally (no double tracer).
 */
UCLASS()
class PAINTFORGE_API UPFWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPFWeaponComponent();   // SetIsReplicatedByDefault(true)

	// ---- Input entry (called by character on owning client) ----
	void StartFire();       // holds; internal 12 bps accumulator w/ remainder carry (04 §2.1)
	void StopFire();
	void StartReload();     // manual; also auto-fires on empty (04 §3)

	/** True while the fire button / bot trigger is held (not replicated; local/authority only). */
	bool WantsFire() const { return bWantsFire; }

	// ---- RPC surface (binding) ----
	UFUNCTION(Server, Reliable)          void ServerFire(const FPFShotPacket& Shot);
	UFUNCTION(NetMulticast, Unreliable)  void MulticastShotFX(FVector_NetQuantize100 Origin,
	                                          FVector_NetQuantizeNormal Dir, uint32 ShotIndex);
		// remote clients spawn a Cosmetic projectile; owning client skips (already fired)
	UFUNCTION(NetMulticast, Unreliable)  void MulticastImpactSplat(FVector_NetQuantize100 Loc,
	                                          FVector_NetQuantizeNormal Normal, uint8 Team);
		// all clients → UPFSplatSubsystem::SpawnConfirmedSplat (owning client reconciles pending @75 uu)
	UFUNCTION(Client, Reliable)          void ClientHitConfirm(uint32 ShotIndex, bool bElimHit);
		// ONLY source of hitmarkers (B3). Broadcasts OnHitConfirmedEvent.

	// ---- Replicated (owner-only correction of predicted values) ----
	/** Balls in the current magazine (0..HopperCapacity). */
	UPROPERTY(ReplicatedUsing=OnRep_Hopper) uint8 HopperCount = 30;   // COND_OwnerOnly
	/** Spare balls carried (not in mag). Mag + reserve ≤ MaxTotalAmmo. */
	UPROPERTY(ReplicatedUsing=OnRep_Reserve) int32 ReserveAmmo = 120; // COND_OwnerOnly
	UPROPERTY(ReplicatedUsing=OnRep_Reload) bool  bReloading  = false; // COND_OwnerOnly

	// ---- Cross-package reads ----
	float GetCurrentSpreadHalfAngleDeg() const;  // live cone incl. bloom + movement state; crosshair polls per tick
	// Spread-seed contract (client & server MUST both use this — B3):
	static FRandomStream MakeShotStream(int32 PlayerId, uint32 ShotIndex);
		// seed = (int32)HashCombine((uint32)PlayerId, ShotIndex); PlayerId = PlayerState::GetPlayerId()

	/** Total balls currently held (mag + reserve). */
	int32 GetTotalAmmo() const { return static_cast<int32>(HopperCount) + ReserveAmmo; }

	/**
	 * Authority: top up mag to capacity and refill reserve to MaxReserveAmmo
	 * (ammo barrel pickup). Returns true if anything was granted.
	 */
	bool ServerRefillFromPickup();

	// ---- UI subscription points ----
	FPFOnHitConfirmed       OnHitConfirmedEvent;
	FPFOnHopperChanged      OnHopperChangedEvent;
	FPFOnReloadStateChanged OnReloadStateChangedEvent;

	// ---- Config (defaults: 30-round mag, 150 total carry) ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float FireRateBps = 12.f;
	/** Magazine size (default 30). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") uint8 HopperCapacity = 30;
	/** Max spare balls outside the mag (default 120 → 150 total with full mag). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") int32 MaxReserveAmmo = 120;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ReloadTime = 1.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float MuzzleSpeedUU = 10000.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ProjGravityScale = 0.35f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ProjLifetime = 2.f;
	/** Hit sphere radius (uu). Default ~1/3 of the original 7 uu BB for tighter airsoft feel. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ProjRadiusUU = 2.33f;
	/** ADS half-angle (deg) — near laser when fully aimed; hipfire stays loose. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadADS = 0.06f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadHip = 1.5f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadHipMoving = 2.0f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadAirAdd = 1.5f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadSlideAdd = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadCrouchMult = 0.8f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomPerShot = 0.12f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomCap = 1.8f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomDecayPerSec = 6.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomDecayDelay = 0.15f;
	/** Bloom retained while ADS (0 = none). Low value = ADS feels much more accurate. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomADSMult = 0.12f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SprintOutTime = 0.18f;

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_Hopper();
	UFUNCTION() void OnRep_Reserve();
	UFUNCTION() void OnRep_Reload();

	// Intra RPC: server must learn about manual reloads (auto-reload triggers independently
	// on both sides when the hopper empties).
	UFUNCTION(Server, Reliable) void ServerStartReload();

private:
	// ---- Fire internals ----
	void TryFire(double Now);           // full gate chain; fires at most one shot
	void FireOneShot(double Now);       // owning-client shot: cosmetic + packet + feel
	bool PassesCommonFireGates(const APaintForgeCharacter& Char) const;   // phase + alive + reload

	// ---- Reload internals (run on owner-predicted client AND on authority) ----
	void BeginReload(double Now);
	void FinishReload();
	void CancelReload();                // sprint/slide: restores nothing — hopper only fills on finish
	void UpdateReload(double Now);

	// ---- Bloom (lazy-decay accumulator; const-readable) ----
	float GetEffectiveBloomDeg(double Now) const;
	void  RegisterShotBloom(double Now);

	// ---- Helpers ----
	APaintForgeCharacter*  GetPFCharacter() const;
	APaintForgeGameState*  GetPFGameState() const;
	uint8 GetOwnerTeam() const;         // 255 if unknown
	void  SpawnCosmeticProjectile(const FVector& Origin, const FVector& SpreadedDir,
	                              uint8 Team, uint32 ShotIndex);

	// Fire state
	bool   bWantsFire = false;
	double NextFireTime = 0.0;
	double SprintOutReadyTime = 0.0;
	uint32 ShotIndexCounter = 0;        // owning-client monotonic (per weapon, per match)

	// Bloom state (updated on every observed shot: local fire / server fire / remote multicast)
	float  BloomAccumDeg = 0.f;
	double LastShotTime = -1000.0;

	// Reload state
	double ReloadEndTime = 0.0;
	bool   bWasADSBeforeReload = false;

	// Server-side validation state
	float  FireTokens = 3.f;            // token bucket, cap 3, refill 12/s (04 §2.1)
	double LastTokenRefillTime = 0.0;
	uint32 LastServerShotIndex = 0;     // monotonicity / replay guard
};
