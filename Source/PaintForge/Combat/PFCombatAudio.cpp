// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFCombatAudio.h"

#include "PaintForge.h"
#include "Player/PaintForgeCharacter.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ConfigCacheIni.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundWaveProcedural.h"

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

	TArray<int16> SynthMuzzle()
	{
		return MixAdd(SynthNoiseBurst(0.028f, 0.55f, 0.45f),
			SynthSineBlip(1800.f, 0.012f, 0.22f, 0.001f, 0.008f));
	}

	TArray<int16> SynthHitmarker()
	{
		return SynthSineBlip(1650.f, 0.05f, 0.42f, 0.001f, 0.028f);
	}

	TArray<int16> SynthElim()
	{
		return Concat(
			SynthSineBlip(980.f, 0.07f, 0.45f, 0.002f, 0.03f),
			SynthSineBlip(620.f, 0.11f, 0.42f, 0.002f, 0.06f));
	}

	TArray<int16> SynthSplatIncoming()
	{
		return MixAdd(SynthSineBlip(140.f, 0.08f, 0.5f, 0.003f, 0.05f),
			SynthNoiseBurst(0.055f, 0.28f, 0.25f), 0.55f);
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
		// Soft mechanical click-clack for hopper flip.
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
			GConfig->GetFloat(TEXT("PaintForge"), TEXT("SfxVolume"), Sfx, GGameUserSettingsIni);
		}
		return FMath::Clamp(Sfx, 0.f, 1.f);
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
	CueElim = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Special_Collectible_26-1_Cue.Special_Collectible_26-1_Cue"));
	CueMuzzle = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Gunshot_7-1_Cue.Gunshot_7-1_Cue"));
	if (CueMuzzle == nullptr)
	{
		CueMuzzle = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Gunshot_1-1_Cue.Gunshot_1-1_Cue"));
	}
	CueSplatIncoming = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Hit_Generic_5-1_Cue.Hit_Generic_5-1_Cue"));
	if (CueSplatIncoming == nullptr)
	{
		CueSplatIncoming = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Hit_Generic_2-1_Cue.Hit_Generic_2-1_Cue"));
	}
	CueBreakout = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Whoosh_4-1_Cue.Whoosh_4-1_Cue"));
	CueDenied = LoadCue(TEXT("/Game/Free_Sounds_Pack/cue/Interface_3-3_Cue.Interface_3-3_Cue"));
	// Build / reload / ready — wood + metal pack pieces that read as physical without sci-fi lasers.
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

	PcmHitmarker = PackPcm(SynthHitmarker());
	PcmElim = PackPcm(SynthElim());
	PcmMuzzle = PackPcm(SynthMuzzle());
	PcmSplatIncoming = PackPcm(SynthSplatIncoming());
	PcmBreakout = PackPcm(SynthBreakout());
	PcmDenied = PackPcm(SynthDenied());
	PcmReload = PackPcm(SynthReload());
	PcmPlace = PackPcm(SynthPlace());
	PcmDelete = PackPcm(SynthDelete());
	PcmReady = PackPcm(SynthReady());

	CombatAttenuation = NewObject<USoundAttenuation>(this);
	{
		FSoundAttenuationSettings& S = CombatAttenuation->Attenuation;
		S.bAttenuate = true;
		S.bSpatialize = true;
		S.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
		S.AttenuationShape = EAttenuationShape::Sphere;
		S.FalloffDistance = 4500.f;
		S.AttenuationShapeExtents = FVector(600.f);
	}

	MuzzleConcurrency = NewObject<USoundConcurrency>(this);
	{
		FSoundConcurrencySettings& C = MuzzleConcurrency->Concurrency;
		C.MaxCount = 6;
		C.bLimitToOwner = false;
		C.ResolutionRule = EMaxConcurrentResolutionRule::StopFarthestThenPreventNew;
		C.RetriggerTime = 0.f;
	}

	UE_LOG(PaintForgeLog, Log,
		TEXT("[Audio] cues: muzzle=%s hit=%s elim=%s splat=%s reload=%s place=%s"),
		CueMuzzle ? TEXT("yes") : TEXT("proc"),
		CueHitmarker ? TEXT("yes") : TEXT("proc"),
		CueElim ? TEXT("yes") : TEXT("proc"),
		CueSplatIncoming ? TEXT("yes") : TEXT("proc"),
		CueReload ? TEXT("yes") : TEXT("proc"),
		CuePlace ? TEXT("yes") : TEXT("proc"));
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
	UGameplayStatics::PlaySound2D(this, Live, Vol, Pitch);
}

void UPFCombatAudio::PlayWorld(USoundBase* Preferred, const TArray<uint8>& Pcm, float Volume, float Pitch,
	USoundConcurrency* Concurrency)
{
	if (!CanPlay())
	{
		return;
	}

	FVector Loc = FVector::ZeroVector;
	if (const APaintForgeCharacter* Char = Cast<APaintForgeCharacter>(GetOwner()))
	{
		Loc = Char->GetMuzzleLocation(Char->IsLocallyControlled());
	}
	else if (const AActor* Owner = GetOwner())
	{
		Loc = Owner->GetActorLocation();
	}

	USoundBase* ToPlay = Preferred;
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
	}

	const float Vol = Volume * ReadSfxVolumeScale();
	if (CombatAttenuation)
	{
		UGameplayStatics::SpawnSoundAtLocation(this, ToPlay, Loc, FRotator::ZeroRotator,
			Vol, Pitch, 0.f, CombatAttenuation, Concurrency);
	}
	else
	{
		UGameplayStatics::PlaySound2D(this, ToPlay, Vol, Pitch);
	}
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
	PlayWorld(CueMuzzle, PcmMuzzle, 0.72f, FMath::FRandRange(0.94f, 1.06f), MuzzleConcurrency);
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
	// World-ish feel but 2D so turbo-build doesn't need a location spam path.
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
