// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFCombatAudio.h"

#include "CombatForge.h"
#include "Core/PFUserPrefs.h"
#include "Player/CombatForgeCharacter.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ConfigCacheIni.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"

namespace
{
	constexpr int32 kSampleRate = 22050;

	TArray<uint8> PackPcm(const TArray<int16>& Samples)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Samples.Num() * sizeof(int16));
		FMemory::Memcpy(Bytes.GetData(), Samples.GetData(), Bytes.Num());
		return Bytes;
	}

	TArray<int16> SynthSineBlip(float FreqHz, float DurationSec, float Amplitude,
		float AttackSec = 0.002f, float ReleaseSec = 0.02f)
	{
		const int32 N = FMath::Max(1, FMath::RoundToInt(DurationSec * kSampleRate));
		TArray<int16> Out;
		Out.SetNumUninitialized(N);
		const int32 AttackN = FMath::Max(1, FMath::RoundToInt(AttackSec * kSampleRate));
		const int32 ReleaseN = FMath::Max(1, FMath::RoundToInt(ReleaseSec * kSampleRate));
		for (int32 i = 0; i < N; ++i)
		{
			const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
			float Env = 1.f;
			if (i < AttackN)
			{
				Env = static_cast<float>(i) / static_cast<float>(AttackN);
			}
			else if (i > N - ReleaseN)
			{
				Env = static_cast<float>(N - i) / static_cast<float>(ReleaseN);
			}
			const float S = FMath::Sin(2.f * PI * FreqHz * t) * Amplitude * Env;
			Out[i] = static_cast<int16>(FMath::Clamp(S, -1.f, 1.f) * 32767.f);
		}
		return Out;
	}

	TArray<int16> SynthNoiseBurst(float DurationSec, float Amplitude, float LowPass = 0.35f)
	{
		const int32 N = FMath::Max(1, FMath::RoundToInt(DurationSec * kSampleRate));
		TArray<int16> Out;
		Out.SetNumUninitialized(N);
		float Prev = 0.f;
		FRandomStream Rng(0xA17F00D);
		for (int32 i = 0; i < N; ++i)
		{
			const float Env = 1.f - static_cast<float>(i) / static_cast<float>(N);
			const float Env2 = Env * Env;
			const float White = Rng.FRandRange(-1.f, 1.f);
			Prev = FMath::Lerp(Prev, White, LowPass);
			const float S = Prev * Amplitude * Env2;
			Out[i] = static_cast<int16>(FMath::Clamp(S, -1.f, 1.f) * 32767.f);
		}
		return Out;
	}

	TArray<int16> Concat(const TArray<int16>& A, const TArray<int16>& B)
	{
		TArray<int16> Out = A;
		Out.Append(B);
		return Out;
	}

	TArray<int16> MixAdd(const TArray<int16>& A, const TArray<int16>& B, float BScale = 1.f)
	{
		const int32 N = FMath::Max(A.Num(), B.Num());
		TArray<int16> Out;
		Out.SetNumZeroed(N);
		for (int32 i = 0; i < A.Num(); ++i) { Out[i] = A[i]; }
		for (int32 i = 0; i < B.Num(); ++i)
		{
			const int32 Mix = static_cast<int32>(Out[i]) + static_cast<int32>(B[i] * BScale);
			Out[i] = static_cast<int16>(FMath::Clamp(Mix, -32767, 32767));
		}
		return Out;
	}

	// Near-field transient: sharp mechanical "pop" (solenoid / valve).
	TArray<int16> SynthMuzzleClose()
	{
		return MixAdd(
			SynthNoiseBurst(0.014f, 0.62f, 0.55f),
			SynthSineBlip(2400.f, 0.008f, 0.28f, 0.0005f, 0.005f), 0.85f);
	}

	// Body: air + mid thump (gas dump).
	TArray<int16> SynthMuzzleBody()
	{
		return MixAdd(
			MixAdd(SynthNoiseBurst(0.038f, 0.48f, 0.42f),
				SynthSineBlip(320.f, 0.04f, 0.28f, 0.001f, 0.025f), 0.75f),
			SynthSineBlip(140.f, 0.05f, 0.22f, 0.002f, 0.035f), 0.55f);
	}

	// Distant tail: low muffled body for spatial distance layering.
	TArray<int16> SynthMuzzleTail()
	{
		return MixAdd(SynthNoiseBurst(0.09f, 0.28f, 0.22f),
			SynthSineBlip(95.f, 0.10f, 0.30f, 0.003f, 0.07f), 0.8f);
	}

	TArray<int16> SynthHitmarker()
	{
		return SynthSineBlip(1650.f, 0.05f, 0.42f, 0.001f, 0.028f);
	}

	TArray<int16> SynthElim()
	{
		return Concat(
			MixAdd(SynthNoiseBurst(0.10f, 0.5f, 0.28f),
				SynthSineBlip(160.f, 0.14f, 0.55f, 0.001f, 0.11f), 0.9f),
			SynthSineBlip(105.f, 0.11f, 0.32f, 0.003f, 0.09f));
	}

	TArray<int16> SynthSplatIncoming()
	{
		return MixAdd(SynthSineBlip(140.f, 0.08f, 0.5f, 0.003f, 0.05f),
			SynthNoiseBurst(0.055f, 0.28f, 0.25f), 0.55f);
	}

	TArray<int16> SynthImpact()
	{
		// Hard surface BB strike: short click + dust body.
		return MixAdd(SynthNoiseBurst(0.04f, 0.42f, 0.48f),
			SynthSineBlip(780.f, 0.018f, 0.18f, 0.0005f, 0.012f), 0.7f);
	}

	TArray<int16> SynthBreakout()
	{
		return Concat(
			SynthSineBlip(440.f, 0.18f, 0.42f, 0.01f, 0.04f),
			SynthSineBlip(660.f, 0.28f, 0.45f, 0.01f, 0.08f));
	}

	TArray<int16> SynthDenied()
	{
		return SynthSineBlip(180.f, 0.12f, 0.42f, 0.002f, 0.04f);
	}

	TArray<int16> SynthReload()
	{
		return Concat(
			SynthSineBlip(420.f, 0.04f, 0.28f, 0.001f, 0.02f),
			SynthNoiseBurst(0.06f, 0.18f, 0.4f));
	}

	TArray<int16> SynthPlace()
	{
		return MixAdd(SynthNoiseBurst(0.04f, 0.35f, 0.5f),
			SynthSineBlip(320.f, 0.03f, 0.25f, 0.001f, 0.02f));
	}

	TArray<int16> SynthDelete()
	{
		return SynthSineBlip(90.f, 0.08f, 0.35f, 0.002f, 0.05f);
	}

	TArray<int16> SynthReady()
	{
		return SynthSineBlip(880.f, 0.06f, 0.3f, 0.002f, 0.03f);
	}

	TArray<int16> SynthFootstep()
	{
		return MixAdd(SynthNoiseBurst(0.035f, 0.32f, 0.55f),
			SynthSineBlip(95.f, 0.04f, 0.22f, 0.001f, 0.025f), 0.7f);
	}

	TArray<int16> SynthFireSelect()
	{
		return MixAdd(SynthNoiseBurst(0.012f, 0.28f, 0.5f),
			SynthSineBlip(950.f, 0.018f, 0.22f, 0.001f, 0.012f), 0.8f);
	}

	TArray<int16> SynthGrenadeThrow()
	{
		return MixAdd(SynthNoiseBurst(0.20f, 0.30f, 0.6f),
			SynthSineBlip(300.f, 0.16f, 0.16f, 0.02f, 0.11f), 0.7f);
	}

	TArray<int16> SynthFragBurst()
	{
		return MixAdd(SynthSineBlip(72.f, 0.36f, 0.62f, 0.002f, 0.22f),
			SynthNoiseBurst(0.30f, 0.55f, 0.5f), 0.85f);
	}

	TArray<int16> SynthSmokeHiss()
	{
		return SynthNoiseBurst(0.7f, 0.22f, 0.75f);
	}

	USoundWaveProcedural* MakeWaveShell(UObject* Outer, float DurationSec)
	{
		USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(Outer, NAME_None, RF_Transient);
		Wave->SetSampleRate(kSampleRate);
		Wave->NumChannels = 1;
		Wave->bLooping = false;
		Wave->bProcedural = true;
		Wave->SoundGroup = SOUNDGROUP_Default;
		Wave->Duration = DurationSec;
		Wave->VirtualizationMode = EVirtualizationMode::Disabled;
		Wave->bCanProcessAsync = false;
		return Wave;
	}

	float ReadSfxVolumeScale()
	{
		float Sfx = 1.f;
		if (GConfig)
		{
			GConfig->GetFloat(TEXT("CombatForge"), TEXT("SfxVolume"), Sfx, GGameUserSettingsIni);
		}
		return FMath::Clamp(Sfx, 0.f, 1.f);
	}

	void ConfigureSpatialAttenuation(USoundAttenuation* Att, float InnerRadius, float Falloff,
		float LpfMinFreq, float LpfMaxFreq)
	{
		if (Att == nullptr)
		{
			return;
		}
		FSoundAttenuationSettings& S = Att->Attenuation;
		S.bAttenuate = true;
		S.bSpatialize = true;
		S.bAttenuateWithLPF = true;
		S.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
		S.AttenuationShape = EAttenuationShape::Sphere;
		S.FalloffDistance = Falloff;
		S.AttenuationShapeExtents = FVector(InnerRadius);
		S.LPFRadiusMin = InnerRadius;
		S.LPFRadiusMax = InnerRadius + Falloff;
		S.LPFFrequencyAtMin = LpfMinFreq;
		S.LPFFrequencyAtMax = LpfMaxFreq;
		S.bEnableOcclusion = false;   // arena geometry occlusion later
		S.SpatializationAlgorithm = ESoundSpatializationAlgorithm::SPATIALIZATION_Default;
	}
}

UPFCombatAudio::UPFCombatAudio()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

bool UPFCombatAudio::CanPlay() const
{
	const UWorld* World = GetWorld();
	return World != nullptr && World->GetNetMode() != NM_DedicatedServer;
}

void UPFCombatAudio::QueuePcm(USoundWaveProcedural* Wave, const TArray<uint8>& Pcm)
{
	if (Wave == nullptr || Pcm.Num() == 0)
	{
		return;
	}
	Wave->ResetAudio();
	Wave->QueueAudio(Pcm.GetData(), Pcm.Num());
}

void UPFCombatAudio::EnsureSounds()
{
	if (bSoundsReady)
	{
		return;
	}
	bSoundsReady = true;

	auto LoadCue = [](const TCHAR* Path) -> USoundBase*
	{
		return LoadObject<USoundBase>(nullptr, Path);
	};

	CueHitmarker = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Interface_1-1_Cue.Interface_1-1_Cue"));
	CueElim = nullptr;   // force procedural "thunk" — pack cue was a coin chime
	// Layered muzzle: primary gunshot body + mechanical secondary + softer distant.
	CueMuzzle = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Gunshot_7-1_Cue.Gunshot_7-1_Cue"));
	if (CueMuzzle == nullptr)
	{
		CueMuzzle = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Gunshot_1-1_Cue.Gunshot_1-1_Cue"));
	}
	CueMuzzleMech = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Draw_Weapon_Metal_1-1_Cue.Draw_Weapon_Metal_1-1_Cue"));
	if (CueMuzzleMech == nullptr)
	{
		CueMuzzleMech = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Whoosh_1-1_Cue.Whoosh_1-1_Cue"));
	}
	// Reuse medium gunshot as spatial tail when available (pitch-down at play time).
	CueMuzzleTail = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Gunshot_1-1_Cue.Gunshot_1-1_Cue"));
	if (CueMuzzleTail == CueMuzzle)
	{
		// Prefer a distinct secondary if both resolved to the same asset.
		CueMuzzleTail = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Fire_Whoosh_2-15_Cue.Fire_Whoosh_2-15_Cue"));
	}

	CueSplatIncoming = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Hit_Generic_5-1_Cue.Hit_Generic_5-1_Cue"));
	if (CueSplatIncoming == nullptr)
	{
		CueSplatIncoming = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Hit_Generic_2-1_Cue.Hit_Generic_2-1_Cue"));
	}
	CueImpact = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Rock_Impact_37_Cue.Rock_Impact_37_Cue"));
	if (CueImpact == nullptr)
	{
		CueImpact = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Rock_Impact_11_Cue.Rock_Impact_11_Cue"));
	}
	CueBreakout = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Whoosh_4-1_Cue.Whoosh_4-1_Cue"));
	CueDenied = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Interface_3-3_Cue.Interface_3-3_Cue"));
	CueReload = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Sci-Fi_Gun_1_Reload_Cue.Sci-Fi_Gun_1_Reload_Cue"));
	if (CueReload == nullptr)
	{
		CueReload = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Draw_Weapon_Metal_1-1_Cue.Draw_Weapon_Metal_1-1_Cue"));
	}
	CuePlace = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Wood_Move_2-1_Cue.Wood_Move_2-1_Cue"));
	if (CuePlace == nullptr)
	{
		CuePlace = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Nail_Wood_1-1_Cue.Nail_Wood_1-1_Cue"));
	}
	CueDelete = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Wood_Chop_1-4_Cue.Wood_Chop_1-4_Cue"));
	if (CueDelete == nullptr)
	{
		CueDelete = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Rock_Impact_11_Cue.Rock_Impact_11_Cue"));
	}
	CueReady = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Special_Collectible_9-1_Cue.Special_Collectible_9-1_Cue"));
	if (CueReady == nullptr)
	{
		CueReady = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Magical_Interface_5-1_Cue.Magical_Interface_5-1_Cue"));
	}
	CueFootstep = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Rock_Impact_11_Cue.Rock_Impact_11_Cue"));
	if (CueFootstep == nullptr)
	{
		CueFootstep = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Wood_14-8_Cue.Wood_14-8_Cue"));
	}
	CueAmbient = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Ambient_Wind_Loop_1_Cue.Ambient_Wind_Loop_1_Cue"));
	if (CueAmbient == nullptr)
	{
		CueAmbient = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Ambient_Birds_Loop_04_Cue.Ambient_Birds_Loop_04_Cue"));
	}

	// Fire selector + grenades — fixed paths that exist in Free_Sounds_Pack.
	CueFireSelect = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Interface_3-3_Cue.Interface_3-3_Cue"));
	CueGrenadeThrow = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Whoosh_4-1_Cue.Whoosh_4-1_Cue"));
	CueFragBurst = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Explosion_Medium_2-1_Cue.Explosion_Medium_2-1_Cue"));
	if (CueFragBurst == nullptr)
	{
		CueFragBurst = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Explosion_Large_1-1_Cue.Explosion_Large_1-1_Cue"));
	}
	// CueSmokeHiss intentionally procedural-only.

	PcmHitmarker = PackPcm(SynthHitmarker());
	PcmElim = PackPcm(SynthElim());
	PcmMuzzleClose = PackPcm(SynthMuzzleClose());
	PcmMuzzle = PackPcm(SynthMuzzleBody());
	PcmMuzzleTail = PackPcm(SynthMuzzleTail());
	PcmSplatIncoming = PackPcm(SynthSplatIncoming());
	PcmImpact = PackPcm(SynthImpact());
	PcmBreakout = PackPcm(SynthBreakout());
	PcmDenied = PackPcm(SynthDenied());
	PcmReload = PackPcm(SynthReload());
	PcmPlace = PackPcm(SynthPlace());
	PcmDelete = PackPcm(SynthDelete());
	PcmReady = PackPcm(SynthReady());
	PcmFootstep = PackPcm(SynthFootstep());
	PcmFireSelect = PackPcm(SynthFireSelect());
	PcmGrenadeThrow = PackPcm(SynthGrenadeThrow());
	PcmFragBurst = PackPcm(SynthFragBurst());
	PcmSmokeHiss = PackPcm(SynthSmokeHiss());

	// Spatial models: NaturalSound + distance LPF (AAA-ish distance mute without full MetaSounds).
	CombatAttenuation = NewObject<USoundAttenuation>(this);
	ConfigureSpatialAttenuation(CombatAttenuation, /*Inner=*/450.f, /*Falloff=*/5200.f,
		/*LPF near=*/18000.f, /*LPF far=*/900.f);

	CloseAttenuation = NewObject<USoundAttenuation>(this);
	ConfigureSpatialAttenuation(CloseAttenuation, /*Inner=*/120.f, /*Falloff=*/900.f,
		/*LPF near=*/20000.f, /*LPF far=*/4000.f);

	DistantAttenuation = NewObject<USoundAttenuation>(this);
	ConfigureSpatialAttenuation(DistantAttenuation, /*Inner=*/900.f, /*Falloff=*/9000.f,
		/*LPF near=*/6000.f, /*LPF far=*/400.f);

	FootstepAttenuation = NewObject<USoundAttenuation>(this);
	ConfigureSpatialAttenuation(FootstepAttenuation, /*Inner=*/180.f, /*Falloff=*/2400.f,
		/*LPF near=*/14000.f, /*LPF far=*/800.f);

	ImpactAttenuation = NewObject<USoundAttenuation>(this);
	ConfigureSpatialAttenuation(ImpactAttenuation, /*Inner=*/200.f, /*Falloff=*/3200.f,
		/*LPF near=*/16000.f, /*LPF far=*/700.f);

	MuzzleConcurrency = NewObject<USoundConcurrency>(this);
	{
		FSoundConcurrencySettings& C = MuzzleConcurrency->Concurrency;
		C.MaxCount = 10;
		C.bLimitToOwner = false;
		// StopOldest: co-located muzzle voices can't use "farthest" eviction (all at the gun).
		C.ResolutionRule = EMaxConcurrentResolutionRule::StopOldest;
		C.RetriggerTime = 0.f;
	}

	ImpactConcurrency = NewObject<USoundConcurrency>(this);
	{
		FSoundConcurrencySettings& C = ImpactConcurrency->Concurrency;
		C.MaxCount = 12;
		C.bLimitToOwner = false;
		C.ResolutionRule = EMaxConcurrentResolutionRule::StopFarthestThenOldest;
		C.RetriggerTime = 0.f;
	}

	// Defense-in-depth for 2D procedural one-shots: even if a future wave slips past StopCompAfter,
	// it evicts an old squatter instead of permanently occupying one of the 32 mixer voices.
	UIConcurrency = NewObject<USoundConcurrency>(this);
	{
		FSoundConcurrencySettings& C = UIConcurrency->Concurrency;
		C.MaxCount = 16;
		C.bLimitToOwner = false;
		C.ResolutionRule = EMaxConcurrentResolutionRule::StopOldest;
		C.RetriggerTime = 0.f;
	}

	UE_LOG(CombatForgeLog, Log,
		TEXT("[Audio] layered muzzle body=%s mech=%s tail=%s impact=%s"),
		CueMuzzle ? TEXT("cue") : TEXT("proc"),
		CueMuzzleMech ? TEXT("cue") : TEXT("proc"),
		CueMuzzleTail ? TEXT("cue") : TEXT("proc"),
		CueImpact ? TEXT("cue") : TEXT("proc"));
}

void UPFCombatAudio::PlayUI(USoundBase* Preferred, const TArray<uint8>& Pcm, float Volume, float Pitch)
{
	if (!CanPlay())
	{
		return;
	}
	const float Vol = Volume * ReadSfxVolumeScale();
	if (Preferred != nullptr)
	{
		UGameplayStatics::PlaySound2D(this, Preferred, Vol, Pitch);
		return;
	}
	if (Pcm.Num() == 0)
	{
		return;
	}
	const float Dur = static_cast<float>(Pcm.Num() / sizeof(int16)) / static_cast<float>(kSampleRate);
	USoundWaveProcedural* Live = MakeWaveShell(this, Dur);
	QueuePcm(Live, Pcm);
	// Fire-and-forget PlaySound2D leaks procedural waves forever (they never self-finish) — the
	// root cause of "gunfire dies after a minute". Spawn a component and stop it ourselves.
	UAudioComponent* Comp = UGameplayStatics::SpawnSound2D(this, Live, Vol, Pitch, 0.f, UIConcurrency);
	StopCompAfter(Comp, Dur / FMath::Max(0.1f, Pitch) + 0.1f);
}

void UPFCombatAudio::PlayWorld(USoundBase* Preferred, const TArray<uint8>& Pcm, float Volume, float Pitch,
	USoundConcurrency* Concurrency, USoundAttenuation* AttenuationOverride)
{
	FVector Loc = FVector::ZeroVector;
	if (const ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(GetOwner()))
	{
		Loc = Char->GetMuzzleLocation(Char->IsLocallyControlled());
	}
	else if (const AActor* Owner = GetOwner())
	{
		Loc = Owner->GetActorLocation();
	}
	PlayWorldAt(Preferred, Pcm, Loc, Volume, Pitch, Concurrency, AttenuationOverride);
}

void UPFCombatAudio::PlayWorldAt(USoundBase* Preferred, const TArray<uint8>& Pcm, const FVector& Loc,
	float Volume, float Pitch, USoundConcurrency* Concurrency, USoundAttenuation* AttenuationOverride)
{
	if (!CanPlay())
	{
		return;
	}
	USoundBase* ToPlay = Preferred;
	float ProcDur = -1.f;   // >0 = procedural wave that must be hard-stopped (never self-finishes)
	if (ToPlay == nullptr)
	{
		if (Pcm.Num() == 0)
		{
			return;
		}
		const float Dur = static_cast<float>(Pcm.Num() / sizeof(int16)) / static_cast<float>(kSampleRate);
		USoundWaveProcedural* Wave = MakeWaveShell(this, Dur);
		QueuePcm(Wave, Pcm);
		ToPlay = Wave;
		ProcDur = Dur;
	}
	const float Vol = Volume * ReadSfxVolumeScale();
	USoundAttenuation* Att = AttenuationOverride ? AttenuationOverride : CombatAttenuation.Get();
	if (Att)
	{
		UAudioComponent* Comp = UGameplayStatics::SpawnSoundAtLocation(this, ToPlay, Loc,
			FRotator::ZeroRotator, Vol, Pitch, 0.f, Att, Concurrency);
		if (ProcDur > 0.f)
		{
			StopCompAfter(Comp, ProcDur / FMath::Max(0.1f, Pitch) + 0.1f);
		}
	}
	else
	{
		UAudioComponent* Comp = UGameplayStatics::SpawnSound2D(this, ToPlay, Vol, Pitch, 0.f,
			ProcDur > 0.f ? UIConcurrency.Get() : nullptr);
		if (ProcDur > 0.f)
		{
			StopCompAfter(Comp, ProcDur / FMath::Max(0.1f, Pitch) + 0.1f);
		}
	}
}

void UPFCombatAudio::StopCompAfter(UAudioComponent* Comp, float Seconds)
{
	UWorld* World = GetWorld();
	if (Comp == nullptr || World == nullptr)
	{
		return;
	}
	// bAutoDestroy is true on spawned one-shots: Stop() releases the active sound, its mixer
	// voice, and its concurrency slot. WeakLambda no-ops if the component died first.
	FTimerHandle Handle;
	World->GetTimerManager().SetTimer(Handle,
		FTimerDelegate::CreateWeakLambda(Comp, [Comp]() { Comp->Stop(); }),
		FMath::Max(0.05f, Seconds), /*bLoop=*/false);
}

void UPFCombatAudio::PlayLayeredMuzzle(const FVector& Loc, bool bLocalOwner)
{
	const float Pitch = FMath::FRandRange(0.94f, 1.06f);
	const float PitchMech = FMath::FRandRange(0.97f, 1.08f);
	const float PitchTail = FMath::FRandRange(0.82f, 0.92f);   // lower = distant body

	if (bLocalOwner)
	{
		// Near-field punch as 2D so own shots never get buried by spatial distance.
		PlayUI(nullptr, PcmMuzzleClose, 0.55f, PitchMech);
		// Body still spatial at muzzle for stereo image when turning.
		PlayWorldAt(CueMuzzle, PcmMuzzle, Loc, 0.58f, Pitch, MuzzleConcurrency, CombatAttenuation);
		// Quiet mechanical layer (valve / bolt fiction).
		if (CueMuzzleMech)
		{
			PlayWorldAt(CueMuzzleMech, PcmMuzzleClose, Loc, 0.22f, PitchMech, MuzzleConcurrency, CloseAttenuation);
		}
	}
	else
	{
		// Remote: full spatial stack — close transient + body + low tail for distance.
		PlayWorldAt(nullptr, PcmMuzzleClose, Loc, 0.45f, PitchMech, MuzzleConcurrency, CloseAttenuation);
		PlayWorldAt(CueMuzzle, PcmMuzzle, Loc, 0.72f, Pitch, MuzzleConcurrency, CombatAttenuation);
		PlayWorldAt(CueMuzzleTail, PcmMuzzleTail, Loc, 0.38f, PitchTail, MuzzleConcurrency, DistantAttenuation);
	}
}

void UPFCombatAudio::PlayFireSelect()
{
	EnsureSounds();
	PlayUI(CueFireSelect, PcmFireSelect, 0.7f, FMath::FRandRange(0.98f, 1.04f));
}

void UPFCombatAudio::PlayGrenadeThrow()
{
	EnsureSounds();
	PlayWorld(CueGrenadeThrow, PcmGrenadeThrow, 0.8f, FMath::FRandRange(0.96f, 1.06f), nullptr);
}

void UPFCombatAudio::PlayFragBurstAt(const FVector& Loc)
{
	EnsureSounds();
	PlayWorldAt(CueFragBurst, PcmFragBurst, Loc, 1.2f, FMath::FRandRange(0.94f, 1.02f),
		nullptr, DistantAttenuation);
}

void UPFCombatAudio::PlaySmokeHissAt(const FVector& Loc)
{
	EnsureSounds();
	PlayWorldAt(CueSmokeHiss, PcmSmokeHiss, Loc, 0.8f, FMath::FRandRange(0.98f, 1.04f),
		nullptr, CombatAttenuation);
}

void UPFCombatAudio::PlayHitmarker()
{
	EnsureSounds();
	PlayUI(CueHitmarker, PcmHitmarker, 1.05f, FMath::FRandRange(0.98f, 1.06f));
}

void UPFCombatAudio::PlayElim()
{
	EnsureSounds();
	PlayUI(CueElim, PcmElim, 1.1f, 1.f);
}

void UPFCombatAudio::PlayMuzzle()
{
	EnsureSounds();
	if (!CanPlay())
	{
		return;
	}

	FVector Loc = FVector::ZeroVector;
	bool bLocal = false;
	if (const ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(GetOwner()))
	{
		bLocal = Char->IsLocallyControlled() && Char->IsPlayerControlled();
		Loc = Char->GetMuzzleLocation(bLocal);
	}
	else if (const AActor* Owner = GetOwner())
	{
		Loc = Owner->GetActorLocation();
		if (const APawn* Pawn = Cast<APawn>(Owner))
		{
			bLocal = Pawn->IsLocallyControlled();
		}
	}

	PlayLayeredMuzzle(Loc, bLocal);
}

void UPFCombatAudio::PlayImpactAt(const FVector& Loc)
{
	EnsureSounds();
	PlayWorldAt(CueImpact, PcmImpact, Loc, 0.55f, FMath::FRandRange(0.92f, 1.08f),
		ImpactConcurrency, ImpactAttenuation);
}

void UPFCombatAudio::PlaySplatIncoming()
{
	EnsureSounds();
	PlayUI(CueSplatIncoming, PcmSplatIncoming, 1.05f, FMath::FRandRange(0.94f, 1.06f));
}

void UPFCombatAudio::PlayBreakout()
{
	EnsureSounds();
	PlayUI(CueBreakout, PcmBreakout, 1.05f, 1.f);
}

void UPFCombatAudio::PlayDenied()
{
	EnsureSounds();
	PlayUI(CueDenied, PcmDenied, 0.9f, 1.f);
}

void UPFCombatAudio::PlayReload()
{
	EnsureSounds();
	PlayUI(CueReload, PcmReload, 0.85f, FMath::FRandRange(0.96f, 1.04f));
}

void UPFCombatAudio::PlayPlace()
{
	EnsureSounds();
	PlayUI(CuePlace, PcmPlace, 0.7f, FMath::FRandRange(0.95f, 1.08f));
}

void UPFCombatAudio::PlayDelete()
{
	EnsureSounds();
	PlayUI(CueDelete, PcmDelete, 0.75f, FMath::FRandRange(0.92f, 1.05f));
}

void UPFCombatAudio::PlayReady()
{
	EnsureSounds();
	PlayUI(CueReady, PcmReady, 0.8f, 1.f);
}

void UPFCombatAudio::PlayFootstep(bool bSprint)
{
	EnsureSounds();
	if (!CanPlay())
	{
		return;
	}
	const float Pitch = bSprint ? FMath::FRandRange(1.05f, 1.18f) : FMath::FRandRange(0.9f, 1.05f);
	// Tom 2026-07-17 "I hate the footstep sound": cut to ~1/3 so it's a subtle scuff, not intrusive.
	// (Morning Q: keep quiet, remove entirely, or swap the cue?)
	const float Vol = (bSprint ? 0.16f : 0.11f) * ReadSfxVolumeScale();

	FVector Loc = FVector::ZeroVector;
	if (const AActor* Owner = GetOwner())
	{
		Loc = Owner->GetActorLocation();
	}

	USoundBase* ToPlay = CueFootstep;
	float ProcDur = -1.f;
	if (ToPlay == nullptr)
	{
		if (PcmFootstep.Num() == 0)
		{
			return;
		}
		const float Dur = static_cast<float>(PcmFootstep.Num() / sizeof(int16)) / static_cast<float>(kSampleRate);
		USoundWaveProcedural* Wave = MakeWaveShell(this, Dur);
		QueuePcm(Wave, PcmFootstep);
		ToPlay = Wave;
		ProcDur = Dur;
	}

	if (FootstepAttenuation)
	{
		UAudioComponent* Comp = UGameplayStatics::SpawnSoundAtLocation(this, ToPlay, Loc,
			FRotator::ZeroRotator, Vol, Pitch, 0.f, FootstepAttenuation, nullptr);
		if (ProcDur > 0.f)
		{
			StopCompAfter(Comp, ProcDur / FMath::Max(0.1f, Pitch) + 0.1f);
		}
	}
	else
	{
		UAudioComponent* Comp = UGameplayStatics::SpawnSound2D(this, ToPlay, Vol, Pitch, 0.f,
			ProcDur > 0.f ? UIConcurrency.Get() : nullptr);
		if (ProcDur > 0.f)
		{
			StopCompAfter(Comp, ProcDur / FMath::Max(0.1f, Pitch) + 0.1f);
		}
	}
}

void UPFCombatAudio::StartAmbientBed()
{
	EnsureSounds();
	if (!CanPlay() || AmbientComp != nullptr)
	{
		return;
	}
	if (CueAmbient == nullptr)
	{
		return;   // no loop without pack — skip silent proc spam
	}
	const float Vol = FPFUserPrefs::GetAmbientVolume() * ReadSfxVolumeScale();
	AmbientComp = UGameplayStatics::SpawnSound2D(this, CueAmbient, Vol, 1.f, 0.f, nullptr, true, false);
	if (AmbientComp)
	{
		AmbientComp->bIsUISound = true;
		AmbientComp->bAllowSpatialization = false;
	}
}

void UPFCombatAudio::StopAmbientBed()
{
	if (AmbientComp)
	{
		AmbientComp->Stop();
		AmbientComp->DestroyComponent();
		AmbientComp = nullptr;
	}
}
