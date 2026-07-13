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
 * Call-site map:
 *   PlayHitmarker()     — feedback widget on hit confirm
 *   PlayElim()          — shooter elim confirm + victim death
 *   PlayMuzzle()        — weapon, per shot (local + remote cosmetic)
 *   PlaySplatIncoming() — health, owning client on paint hit taken
 *   PlayBreakout()      — PC on round Live (T6 horn)
 *   PlayDenied()        — build placement denial
 *   PlayReload()        — weapon reload start (local)
 *   PlayPlace()         — build place (local optimistic)
 *   PlayDelete()        — build delete (local optimistic)
 *   PlayReady()         — ready toggle (optional UI)
 */
UCLASS()
class PAINTFORGE_API UPFCombatAudio : public UActorComponent
{
	GENERATED_BODY()

public:
	UPFCombatAudio();

	void PlayHitmarker();
	void PlayElim();
	void PlayMuzzle();
	void PlaySplatIncoming();
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
		USoundConcurrency* Concurrency);
	/** Like PlayWorld but at an explicit world location (detonations happen away from the owner). */
	void PlayWorldAt(USoundBase* Preferred, const TArray<uint8>& Pcm, const FVector& Loc, float Volume,
		float Pitch, USoundConcurrency* Concurrency);

	static void QueuePcm(USoundWaveProcedural* Wave, const TArray<uint8>& Pcm);

	// Imported Free_Sounds_Pack cues (null if pack missing).
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueHitmarker;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueElim;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueMuzzle;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueSplatIncoming;
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

	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> CombatAttenuation;
	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> FootstepAttenuation;
	UPROPERTY(Transient) TObjectPtr<USoundConcurrency> MuzzleConcurrency;
	UPROPERTY(Transient) TObjectPtr<UAudioComponent> AmbientComp;

	TArray<uint8> PcmHitmarker;
	TArray<uint8> PcmElim;
	TArray<uint8> PcmMuzzle;
	TArray<uint8> PcmSplatIncoming;
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
