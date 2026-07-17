// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Math/RandomStream.h"
#include "Core/CombatForgeTypes.h"
#include "PFWeaponComponent.generated.h"

class ACombatForgeCharacter;
class ACombatForgeGameState;
class ACombatForgePlayerState;
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
class COMBATFORGE_API UPFWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPFWeaponComponent();   // SetIsReplicatedByDefault(true)

	// ---- Input entry (called by character on owning client) ----
	void StartFire();       // holds; internal 12 bps accumulator w/ remainder carry (04 §2.1)
	void StopFire();
	void StartReload();     // manual; also auto-fires on empty (04 §3)

	/** Fire selector: cycle Single -> Burst -> Auto -> Single. Client-local feel only (no replication). */
	void CycleFireMode();
	EPFFireMode GetFireMode() const { return CurrentFireMode; }
	/** Restrict the fire selector to a weapon's allowed modes (bitmask 1<<EPFFireMode) + set its default. */
	void SetAllowedFireModes(uint8 Mask, EPFFireMode Default);
	void SetFireMode(EPFFireMode Mode);   // clamps to the allowed mask
	bool IsFireModeAllowed(EPFFireMode Mode) const { return (AllowedFireModeMask & (1u << static_cast<uint8>(Mode))) != 0; }

	/** Throw a grenade of the given type (owner-predicts -> ServerThrowGrenade validates + spawns). */
	void StartThrow(EPFGrenadeType Type);
	uint8 GetFragCount()  const { return FragCount; }
	uint8 GetSmokeCount() const { return SmokeCount; }

	/** True while the fire button / bot trigger is held (not replicated; local/authority only). */
	bool WantsFire() const { return bWantsFire; }

	// ---- RPC surface (binding) ----
	UFUNCTION(Server, Reliable)          void ServerFire(const FPFShotPacket& Shot);
	UFUNCTION(Server, Reliable)          void ServerThrowGrenade(FVector_NetQuantize100 Origin,
	                                          FVector_NetQuantizeNormal AimDir, uint8 Type);
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
	/** Grenades carried this life (COND_OwnerOnly). Reset on spawn, topped up at ammo barrels. */
	UPROPERTY(ReplicatedUsing=OnRep_Grenades) uint8 FragCount  = 2;
	UPROPERTY(ReplicatedUsing=OnRep_Grenades) uint8 SmokeCount = 2;

	// ---- Cross-package reads ----
	float GetCurrentSpreadHalfAngleDeg() const;  // live cone incl. bloom + movement state; crosshair polls per tick
	float GetSpreadHalfAngleDeg(float StampT) const;   // per-shot cone with bloom keyed to the shot's stamp (deterministic client+server)
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

	/** Authority: full loadout reset (mag + reserve + grenades) — called on (re)spawn so a corpse-reused pawn
	 *  starts fresh, since the weapon's BeginPlay does not re-run when the pawn is reused. */
	void ServerResetLoadout();

	// ---- UI subscription points ----
	FPFOnHitConfirmed        OnHitConfirmedEvent;
	FPFOnHopperChanged       OnHopperChangedEvent;
	FPFOnReloadStateChanged  OnReloadStateChangedEvent;
	FPFOnFireModeChanged     OnFireModeChangedEvent;
	FPFOnGrenadeCountChanged OnGrenadeCountChangedEvent;

	// ---- Config (defaults: 30-round mag, 150 total carry) ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float FireRateBps = 12.f;
	/** Magazine size (default 30). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") uint8 HopperCapacity = 30;
	/** Max spare balls outside the mag (default 120 → 150 total with full mag). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") int32 MaxReserveAmmo = 120;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ReloadTime = 1.f;
	/** Fire selector: rounds emitted per trigger pull in Burst mode. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") uint8 BurstCount = 3;
	/** Grenade loadout granted per life (also the ammo-barrel refill cap). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") uint8 MaxFrag  = 2;
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") uint8 MaxSmoke = 2;
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
	// Recoil bloom ("halo" pattern): the first BloomFreeShots of a consecutive burst are flat, then every extra
	// shot adds BloomPerShot to the cone up to BloomCap; a BloomResetGap pause (trigger release) resets it.
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomPerShot = 0.15f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomCap = 2.0f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") uint8 BloomFreeShots = 5;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomResetGap = 0.25f;   // ~3 intervals at 12 bps: a real release, not auto cadence
	/** Bloom retained while ADS. High enough that sustained ADS auto fire visibly sprays (burst discipline pays). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomADSMult = 0.5f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SprintOutTime = 0.18f;
	// Sustained-fire recoil CLIMB (human players only): holding the trigger walks the aim up and slightly
	// right per shot; releasing eases it back to where aim started. Punishes trigger-squeezing — you can't
	// hold auto and stay perfectly on target. Bots skip this (their aim-error model plays that role).
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ClimbPitchPerShotDeg  = 0.30f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ClimbYawPerShotDeg    = 0.12f;   // drifts right
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ClimbFreeShotsMult    = 0.4f;    // gentler during the first BloomFreeShots
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ClimbRecoverDegPerSec = 14.f;

	// Recoil-kick feel (camera view-punch + viewmodel kick only; not the spread cone).
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ADSRecoilMult    = 0.4f;   // kick x this when fully aimed
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") uint8 RecoilRampFreeShots = 5;   // first N shots of a mag stay low
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float RecoilRampLowMult = 0.5f;  // kick x during the free window
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float RecoilRampHighMult = 1.0f; // kick x at full ramp
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") uint8 RecoilRampShots   = 10;    // shots to lerp low->high after free

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_Hopper();
	UFUNCTION() void OnRep_Reserve();
	UFUNCTION() void OnRep_Reload();
	UFUNCTION() void OnRep_Grenades();

	// Intra RPC: server must learn about manual reloads (auto-reload triggers independently
	// on both sides when the hopper empties).
	UFUNCTION(Server, Reliable) void ServerStartReload();

private:
	// ---- Fire internals ----
	void TryFire(double Now);           // full gate chain; fires at most one shot
	void FireOneShot(double Now);       // owning-client shot: cosmetic + packet + feel
	bool PassesCommonFireGates(const ACombatForgeCharacter& Char) const;   // phase + alive + reload

	// ---- Reload internals (run on owner-predicted client AND on authority) ----
	void BeginReload(double Now);
	void FinishReload();
	void CancelReload();                // sprint/slide: restores nothing — hopper only fills on finish
	void UpdateReload(double Now);

	// ---- Bloom (consecutive-shot counter, clocked by the SHOOTER's stamp — FPFShotPacket.ClientTime — so the
	//      owning client and the server evaluate bit-identical operands and agree on every shot's cone) ----
	float GetBloomDegForStamp(float StampT) const;   // cone contribution for a shot fired at StampT
	void  AdvanceBloom(float StampT);                // count the shot at StampT (resets after BloomResetGap)

	// ---- Helpers ----
	ACombatForgeCharacter*  GetPFCharacter() const;
	ACombatForgeGameState*  GetPFGameState() const;
	uint8 GetOwnerTeam() const;         // 255 if unknown
	void  SpawnCosmeticProjectile(const FVector& Origin, const FVector& SpreadedDir,
	                              uint8 Team, uint32 ShotIndex);

	// Fire state
	bool   bWantsFire = false;
	double NextFireTime = 0.0;
	double SprintOutReadyTime = 0.0;
	uint32 ShotIndexCounter = 0;        // owning-client monotonic (per weapon, per match)
	EPFFireMode CurrentFireMode = EPFFireMode::Auto;   // client-local fire selector (not replicated)
	uint8 AllowedFireModeMask = (1 << 0) | (1 << 1) | (1 << 2);   // which modes the selector may cycle (per weapon)
	uint8  ShotsThisPull = 0;           // shots emitted since the current trigger press (Single/Burst latch)
	uint8  ShotsThisMag = 0;            // shots since the mag was last filled (recoil ramp; client-local feel)

	// Bloom state (advanced on every observed shot: local fire / server fire / remote multicast). Keyed off the
	// shooter's clock so client + server chains match; remote viewers estimate with their own clock (cosmetic).
	uint16 ConsecShots = 0;
	float  LastShotStampT = -1000.f;

	// Recoil-climb state (owning human player only): how much accumulated climb is still owed back.
	float  RecoilClimbPitch = 0.f;
	float  RecoilClimbYaw = 0.f;
	double LastClimbShotTime = -1000.0;

	// Reload state
	double ReloadEndTime = 0.0;

	// Server-side validation state
	float  FireTokens = 3.f;            // token bucket, cap 3, refill 12/s (04 §2.1)
	double LastTokenRefillTime = 0.0;
	uint32 LastServerShotIndex = 0;     // monotonicity / replay guard
	// ClientTime drives the bloom clock, so the server sanity-checks it: strictly increasing, and the claimed
	// inter-shot gap can't exceed the server-observed gap (a forged big gap would be a free accuracy reset).
	float  LastAcceptedClientTime = -1000.f;
	double LastServerAcceptTime = -1000.0;
};
