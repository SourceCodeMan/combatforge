// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PFCombatAudio.generated.h"

class USoundBase;
class USoundWaveProcedural;
class USoundAttenuation;
class USoundConcurrency;

/**
 * Combat audio hooks (04 §4, contract §3.4). Non-replicated — every call site is already
 * client-correct. Prefers Free_Sounds_Pack cues when present; falls back to procedural
 * one-shots so a checkout without the pack still has audio.
 *
 * Call-site map:
 *   PlayHitmarker()     — UPFCombatFeedbackWidget on hit confirm
 *   PlayElim()          — UPFCombatFeedbackWidget on own elim confirm
 *   PlayMuzzle()        — UPFWeaponComponent, per shot (local + remote cosmetic)
 *   PlaySplatIncoming() — UPFHealthComponent, owning client on paint hit taken
 *   PlayBreakout()      — PC on round Live (T6 horn)
 *   PlayDenied()        — UPFBuildComponent on placement denial
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

private:
	bool CanPlay() const;
	void EnsureSounds();

	/** Prefer imported cue; else procedural PCM. */
	void PlayUI(USoundBase* Preferred, USoundWaveProcedural* Wave, const TArray<uint8>& Pcm,
		float Volume, float Pitch);
	void PlayWorld(USoundBase* Preferred, USoundWaveProcedural* /*Wave*/, const TArray<uint8>& Pcm,
		float Volume, float Pitch, USoundConcurrency* Concurrency);

	static void QueuePcm(USoundWaveProcedural* Wave, const TArray<uint8>& Pcm);

	// Imported Free_Sounds_Pack cues (null if pack missing).
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueHitmarker;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueElim;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueMuzzle;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueSplatIncoming;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueBreakout;
	UPROPERTY(Transient) TObjectPtr<USoundBase> CueDenied;

	// Procedural fallbacks.
	UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> SndHitmarker;
	UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> SndElim;
	UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> SndMuzzle;
	UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> SndSplatIncoming;
	UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> SndBreakout;
	UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> SndDenied;

	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> CombatAttenuation;
	UPROPERTY(Transient) TObjectPtr<USoundConcurrency> MuzzleConcurrency;

	TArray<uint8> PcmHitmarker;
	TArray<uint8> PcmElim;
	TArray<uint8> PcmMuzzle;
	TArray<uint8> PcmSplatIncoming;
	TArray<uint8> PcmBreakout;
	TArray<uint8> PcmDenied;

	bool bSoundsReady = false;
};
