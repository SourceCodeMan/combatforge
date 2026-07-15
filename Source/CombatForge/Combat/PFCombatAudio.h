// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PFCombatAudio.generated.h"

class UAudioComponent;
class USoundBase;
class USoundWaveProcedural;
class USoundAttenuation;
class USoundConcurrency;

/**
 * Combat / UI audio hooks (04 §4, contract §3.4). Non-replicated — every call site is
 * already client-correct. Prefers Free_Sounds_Pack cues when present; falls back to
 * procedural one-shots so a checkout without the pack still has audio.
 *
 * AAA-feel layering:
 *   PlayMuzzle()     — close transient + body + optional tail (2D near / spatial far)
 *   PlayImpactAt()   — world hit thump at splat location (spatial)
 *   Attenuation      — natural falloff + distance LPF (not linear drop)
 *   Concurrency      — muzzle voice cap (StopOldest) so auto-fire never goes silent
 *
 * Call-site map:
 *   PlayHitmarker()     — feedback widget on hit confirm
 *   PlayElim()          — shooter elim confirm + victim death
 *   PlayMuzzle()        — weapon, per shot (local + remote cosmetic)
 *   PlaySplatIncoming() — health, owning client on paint hit taken
 *   PlayImpactAt()      — splat subsystem world hit (optional juice)
 *   PlayBreakout()      — PC on round Live (T6 horn)
 *   PlayDenied()        — build placement denial
 *   PlayReload()        — weapon reload start (local)
 *   PlayPlace()         — build place (local optimistic)
 *   PlayDelete()        — build delete (local optimistic)
 *   PlayReady()         — ready toggle (optional UI)
 */
UCLASS()
class COMBATFORGE_API UPFCombatAudio : public UActorComponent
{
	GENERATED_BODY()

public:
	UPFCombatAudio();

	void PlayHitmarker();
	void PlayElim();
	void PlayMuzzle();
	void PlaySplatIncoming();
	/** World impact thump at a splat location (spatial). Safe no-op if silent/dedi. */
	void PlayImpactAt(const FVector& Loc);
	void PlayBreakout();
	void PlayDenied();
	void PlayReload();
	void PlayPlace();
	void PlayDelete();
	void PlayReady();
	void PlayFootstep(bool bSprint);
	void PlayFireSelect();                          // fire-selector cycle click (2D)
	void PlayGrenadeThrow();                         // throw whoosh (world, at thrower)
	void PlayFragBurstAt(const FVector& Loc);        // frag detonation boom (world, at blast)
	void PlaySmokeHissAt(const FVector& Loc);        // smoke pop hiss (world, at cloud)
	/** Soft arena bed (local only). Idempotent start; stop on end play. */
	void StartAmbientBed();
	void StopAmbientBed();

private:
	bool CanPlay() const;
	void EnsureSounds();

	/** Prefer imported cue; else procedural PCM. */
	void PlayUI(USoundBase* Preferred, const TArray<uint8>& Pcm, float Volume, float Pitch);
	void PlayWorld(USoundBase* Preferred, const TArray<uint8>& Pcm, float Volume, float Pitch,
		USoundConcurrency* Concurrency, USoundAttenuation* AttenuationOverride = nullptr);
	/** Like PlayWorld but at an explicit world location (detonations happen away from the owner). */
	void PlayWorldAt(USoundBase* Preferred, const TArray<uint8>& Pcm, const FVector& Loc, float Volume,
		float Pitch, USoundConcurrency* Concurrency, USoundAttenuation* AttenuationOverride = nullptr);
	/** Layered muzzle: near-field 2D transient + spatial body (and optional tail). */
	void PlayLayeredMuzzle(const FVector& Loc, bool bLocalOwner);

	/** Procedural waves never report finished (engine zero-fills on underrun), so their active
	 *  sounds are immortal — hard-stop the spawned component after the wave's real duration.
	 *  Without this, leaked 2D shot-pops saturate the 32-voice pool after ~1 min of firing and
	 *  ALL gunfire goes silent (louder self-finishing cues like the hitmarker keep working). */
	void StopCompAfter(UAudioComponent* Comp, float Seconds);

	static void QueuePcm(USoundWaveProcedural* Wave, const TArray<uint8>& Pcm);

	// Imported Free_Sounds_Pack cues (null if pack missing).
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueHitmarker;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueElim;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueMuzzle;          // primary body
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueMuzzleMech;      // secondary mechanical layer
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueMuzzleTail;      // distant/low tail
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueSplatIncoming;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueImpact;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueBreakout;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueDenied;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueReload;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CuePlace;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueDelete;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueReady;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueFootstep;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueAmbient;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueFireSelect;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueGrenadeThrow;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueFragBurst;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueSmokeHiss;

	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> CombatAttenuation;     // mid combat (muzzle body)
	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> CloseAttenuation;      // near transient
	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> DistantAttenuation;    // tail / boom
	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> FootstepAttenuation;
	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> ImpactAttenuation;
	UPROPERTY(Transient) TObjectPtr<USoundConcurrency> MuzzleConcurrency;
	UPROPERTY(Transient) TObjectPtr<USoundConcurrency> ImpactConcurrency;
	UPROPERTY(Transient) TObjectPtr<USoundConcurrency> UIConcurrency;    // 2D procedural one-shots: self-cap so a leak can never squat the voice pool
	UPROPERTY(Transient) TObjectPtr<UAudioComponent> AmbientComp;

	TArray<uint8> PcmHitmarker;
	TArray<uint8> PcmElim;
	TArray<uint8> PcmMuzzle;           // mixed layered procedural (body)
	TArray<uint8> PcmMuzzleClose;      // sharp near transient
	TArray<uint8> PcmMuzzleTail;       // low distant body
	TArray<uint8> PcmSplatIncoming;
	TArray<uint8> PcmImpact;
	TArray<uint8> PcmBreakout;
	TArray<uint8> PcmDenied;
	TArray<uint8> PcmReload;
	TArray<uint8> PcmPlace;
	TArray<uint8> PcmDelete;
	TArray<uint8> PcmReady;
	TArray<uint8> PcmFootstep;
	TArray<uint8> PcmFireSelect;
	TArray<uint8> PcmGrenadeThrow;
	TArray<uint8> PcmFragBurst;
	TArray<uint8> PcmSmokeHiss;

	bool bSoundsReady = false;
};
