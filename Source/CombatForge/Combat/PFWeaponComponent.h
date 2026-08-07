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
	/** Ship the local selector to the server (no-op unless locally controlled). (P2-CB6) */
	void PushFireModeToServer();
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
	UFUNCTION(Client, Unreliable)        void ClientHitConfirm(uint32 ShotIndex, bool bElimHit);
		// ONLY source of hitmarkers (B3). Broadcasts OnHitConfirmedEvent. Unreliable since P2-CB3:
		// a lost hitmarker is cosmetic noise; a reliable-channel flood from a frag/bomb burst isn't.

	/** P2-CB3: server-side entry every authoritative ball routes through — coalesces the burst
	 *  (shotgun volley / frag cloud / bomb spray) to ≤1 hitmarker per 50 ms. Elim confirms always
	 *  pass. Call this, not ClientHitConfirm, from impact resolves. */
	void SendHitConfirmCoalesced(uint32 ShotIndex, bool bElimHit);

	/** Abort a reload mid-flight without filling the hopper. Public since P2-CB5: weapon swap
	 *  must kill the outgoing gun's reload or FinishReload fills the INCOMING gun's mag (and the
	 *  leftover bReloading fire-gates it). Also the sprint/slide cancel. */
	void CancelReload();

	// ---- Replicated (owner-only correction of predicted values) ----
	/** Balls in the current magazine (0..HopperCapacity). */
	UPROPERTY(ReplicatedUsing=OnRep_Hopper) uint8 HopperCount = 30;   // COND_OwnerOnly
	/** Spare balls carried (not in mag). Mag + reserve ≤ MaxTotalAmmo. */
	UPROPERTY(ReplicatedUsing=OnRep_Reserve) int32 ReserveAmmo = 120; // COND_OwnerOnly
	UPROPERTY(ReplicatedUsing=OnRep_Reload) bool  bReloading  = false; // COND_OwnerOnly
	/** Grenades carried this life (COND_OwnerOnly). Reset on spawn, topped up at ammo barrels. */
	UPROPERTY(ReplicatedUsing=OnRep_Grenades) uint8 FragCount  = 6;
	UPROPERTY(ReplicatedUsing=OnRep_Grenades) uint8 SmokeCount = 6;

	// ---- Cross-package reads ----
	float GetCurrentSpreadHalfAngleDeg() const;  // live cone incl. bloom + movement state; crosshair polls per tick
	float GetSpreadHalfAngleDeg(float StampT) const;   // per-shot cone with bloom keyed to the shot's stamp (deterministic client+server)
	// Spread-seed contract (client & server MUST both use this — B3):
	static FRandomStream MakeShotStream(int32 PlayerId, uint32 ShotIndex);
		// seed = (int32)HashCombine((uint32)PlayerId, ShotIndex); PlayerId = PlayerState::GetPlayerId()

	/** Total balls currently held (mag + reserve). */
	int32 GetTotalAmmo() const { return static_cast<int32>(HopperCount) + ReserveAmmo; }

	/** Re-broadcast the mag/reserve so the HUD refreshes after a direct ammo write (e.g. a weapon swap on the
	 *  listen host, where no OnRep fires). Owning remote clients refresh through the ammo OnReps. */
	void NotifyAmmoChanged() { OnHopperChangedEvent.Broadcast(HopperCount); }

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
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") uint8 MaxFrag  = 6;
	UPROPERTY(EditDefaultsOnly, Category="PF|Grenade") uint8 MaxSmoke = 6;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float MuzzleSpeedUU = 10000.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ProjGravityScale = 0.35f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ProjLifetime = 2.f;
	/** Hit sphere radius (uu). Default ~1/3 of the original 7 uu BB for tighter airsoft feel. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ProjRadiusUU = 2.33f;
	/** ADS half-angle (deg) — near laser when fully aimed; hipfire stays loose. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadADS = 0.06f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadHip = 1.5f;
	/** Hip cone while walking / strafing (speed above idle, below run). Mild penalty. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadHipMoving = 2.0f;
	/** Hip cone while running / sprint-pace (or still carrying sprint velocity after sprint-out). Heavy penalty. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadHipRunning = 3.5f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadAirAdd = 1.5f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadSlideAdd = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpreadCrouchMult = 0.8f;
	// Recoil bloom ("halo" pattern): continuous decay between shots; full reset after BloomResetGap.
	// Negative BloomPerShot (minigun) tightens the cone; BloomCap is then a FLOOR on total spread contribution.
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomPerShot = 0.15f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomCap = 2.0f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") uint8 BloomFreeShots = 5;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomResetGap = 0.25f;   // full chain reset after a real release
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomDecayDegPerSec = 6.f;
	/** Delay before continuous decay starts after the last shot (stamp-relative). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float BloomDecayStartSec = 0.15f;
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

	/** Region-counter credit per BB (server ApplyPaintHit). Snipers 5–8, most guns 1. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") uint8 HitValue = 1;
	/** >1 = shotgun volley (one ammo/packet, N projectiles with sub-seeds — B3). */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") uint8 Pellets = 1;
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float PelletSpreadDeg = 0.f;
	/** Minigun spin-up before first BB; 0 = no spin. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float SpinupSec = 0.f;
	/** After a full burst, block the next burst for this long (Rifle 03 DMR cadence). 0 = none. */
	UPROPERTY(EditDefaultsOnly, Category="PF|Marker") float ReburstDelaySec = 0.f;

	/** Crosshair halo: current bloom deg at wall-clock now (client display only). */
	float GetCurrentBloomDeg() const;
	float GetBloomCapDeg() const { return BloomCap; }

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
	/** Owning client tells the server which way its selector is set, so the server can enforce the
	 *  cadence that mode implies. Re-clamped server-side to AllowedFireModeMask. (P2-CB6) */
	UFUNCTION(Server, Reliable) void ServerSetFireMode(EPFFireMode Mode);

private:
	// ---- Fire internals ----
	void TryFire(double Now);           // full gate chain; fires at most one shot
	void FireOneShot(double Now);       // owning-client shot: cosmetic + packet + feel
	bool PassesCommonFireGates(const ACombatForgeCharacter& Char) const;   // phase + alive + reload

	// ---- Reload internals (run on owner-predicted client AND on authority) ----
	void BeginReload(double Now);
	void FinishReload();
	void UpdateReload(double Now);

	// ---- Bloom (continuous value + free-shot ramp, clocked by the SHOOTER's stamp — FPFShotPacket.ClientTime) ----
	float GetBloomDegForStamp(float StampT) const;   // cone contribution for a shot fired at StampT (pre-advance)
	void  AdvanceBloom(float StampT);                // apply this shot to the bloom chain
	/** Decay bloom over a stamp gap (full reset if gap > BloomResetGap). Deterministic. */
	float DecayBloomOverGap(float Bloom, float GapSec) const;

	// ---- Helpers ----
	ACombatForgeCharacter*  GetPFCharacter() const;
	ACombatForgeGameState*  GetPFGameState() const;
	uint8 GetOwnerTeam() const;         // 255 if unknown
	void  SpawnCosmeticProjectile(const FVector& Origin, const FVector& SpreadedDir,
	                              uint8 Team, uint32 ShotIndex);
	/** Pellet fan around BaseSpreadedDir using HashCombine(ShotSeed, PelletIdx) — never extra VRandCone on main stream. */
	FVector PelletDir(const FVector& BaseSpreadedDir, uint32 ShotSeed, int32 PelletIdx) const;
	void SpawnPelletVolley(const FVector& Origin, const FVector& BaseSpreadedDir, uint8 Team,
	                       uint32 ShotIndex, int32 PlayerId, bool bAuthoritative, ACombatForgeCharacter* Char);

	// Fire state
	bool   bWantsFire = false;
	/** Burst: a single trigger press commits the full BurstCount even after release (tap = 3-round burst). */
	bool   bBurstCommit = false;
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
	float  BloomCurrent = 0.f;   // bloom AFTER last AdvanceBloom (pre-decay for the next gap)

	// Recoil-climb state (owning human player only): how much accumulated climb is still owed back.
	float  RecoilClimbPitch = 0.f;
	float  RecoilClimbYaw = 0.f;
	double LastClimbShotTime = -1000.0;
	/** Server-side hitmarker coalescing window (P2-CB3, see SendHitConfirmCoalesced). */
	double LastHitConfirmSentAt = -1000.0;

	// Spin-up (minigun) — client gate; server enforces the same def-driven delay via packet spacing + this gate on host.
	double SpinReadyTime = 0.0;
	double SpinGraceUntil = 0.0;

	// Burst DMR re-burst delay (Rifle 03).
	double NextBurstAllowedTime = 0.0;

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
	// Consecutive accepted shots < 0.3 s apart (client clock) — drives the server-side DMR re-burst gate
	// (issue #11 CB2). Reset to 1 on any real pause.
	uint8  ServerBurstRun = 0;
	/** Server's copy of the owning client's fire selector (P2-CB6). CurrentFireMode never leaves the
	 *  client, so before this the server had no idea a weapon was on Single and happily accepted a
	 *  full-auto stream from a modified client. Pushed by ServerSetFireMode and re-clamped to the
	 *  weapon's AllowedFireModeMask (catalog truth the server sets itself in ApplyWeaponLoadout). */
	EPFFireMode ServerFireMode = EPFFireMode::Auto;
};
